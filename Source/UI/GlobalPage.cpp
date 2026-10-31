// GlobalPage.cpp — the GLOBAL page (spec E §6): Preferences (app-wide,
// Settings), VOICE CONTROL (voicing, portamento, pitch bend ranges), QUALITY
// (Good / High / Ultra oversampling) and TUNING (concert A, main tuning, TUN
// file menu). Items with no backing data are shown disabled.
#include "FxModules.h"

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

namespace tincture::ui
{
using namespace juce;

namespace
{
// One preference: a two-option segmented switch or a checkbox.
class PrefRow final : public Component, public Refreshable
{
public:
    PrefRow (PanelContext& c, const String& l, StringArray opts, std::function<bool()> g, std::function<void (bool)> s, const String& tip = {})
        : ctx (c), label (l), get (std::move (g)), set (std::move (s))
    {
        ctx.add (this);
        const bool on = static_cast<bool> (set);
        if (opts.size() == 2)
        {
            for (int i = 0; i < 2; ++i)
            {
                auto b = std::make_unique<PanelButton> (opts[i]);
                b->setFontHeight (10.5f);
                b->setLitColour (colours::ledYellow);
                b->onClick = [this, i] (const ModifierKeys&) { if (set) { set (i == 1); refresh(); } };
                b->setDimmed (! on);
                b->setEnabled (on);
                addAndMakeVisible (*b);
                buttons.push_back (std::move (b));
            }
        }
        else
        {
            auto b = std::make_unique<PanelButton> ("");
            b->setLitColour (colours::ledYellow);
            b->onClick = [this] (const ModifierKeys&) { if (set) { set (! get()); refresh(); } };
            b->setDimmed (! on);
            b->setEnabled (on);
            addAndMakeVisible (*b);
            buttons.push_back (std::move (b));
            checkbox = true;
        }
        setTooltip (tip.isNotEmpty() ? tip : (on ? String() : String ("Not available in Tincture yet")));
        refresh();
    }
    ~PrefRow() override { ctx.remove (this); }
    void setTooltip (const String& t) { for (auto& b : buttons) b->setTooltip (t); }
    void refresh() override
    {
        const bool v = get ? get() : false;
        if (! set)
        {
            for (auto& b : buttons) b->setLit (false);    // no backing data: show nothing selected
            return;
        }
        if (checkbox) buttons[0]->setLit (v);
        else { buttons[0]->setLit (! v); buttons[1]->setLit (v); }
    }
    void paint (Graphics& g) override
    {
        const bool on = static_cast<bool> (set);
        auto r = getLocalBounds().toFloat();
        if (checkbox) r.removeFromLeft (26.0f);
        drawText (g, label, r.withTrimmedRight (checkbox ? 0.0f : 150.0f), 12.0f, on ? colours::ink : colours::inkFaint, false, Justification::centredLeft);
    }
    void resized() override
    {
        if (checkbox) buttons[0]->setBounds (0, (getHeight() - 18) / 2, 18, 18);
        else
        {
            buttons[0]->setBounds (getWidth() - 146, 2, 72, getHeight() - 4);
            buttons[1]->setBounds (getWidth() - 72, 2, 72, getHeight() - 4);
        }
    }

private:
    PanelContext& ctx;
    String label;
    std::function<bool()> get;
    std::function<void (bool)> set;
    std::vector<std::unique_ptr<PanelButton>> buttons;
    bool checkbox = false;
};

class GlobalPage final : public Component, public Refreshable
{
public:
    explicit GlobalPage (PanelContext& c) : ctx (c), proc (c.proc)
    {
        ctx.add (this);
        auto& s = proc.settings;
        auto& st = s;
        auto header = [this] (const String& t) { headers.push_back ({ t, (int) rows.size() }); };
        auto row = [this] (const String& l, StringArray o, std::function<bool()> g, std::function<void (bool)> fn, const String& tip = {})
        {
            rows.push_back (std::make_unique<PrefRow> (ctx, l, o, std::move (g), std::move (fn), tip));
            addAndMakeVisible (*rows.back());
        };
        const StringArray hs { "HIDE", "SHOW" }, oo { "OFF", "ON" };
        header ("USER INTERFACE");
        row ("Help tooltips", hs, [&st] { return st.tooltips(); }, [&st] (bool v) { st.setTooltips (v); });
        row ("Param value tooltips", hs, [] { return true; }, {});
        row ("Double-click params", { "RESET", "TYPE VALUE" }, [&st] { return ! st.doubleClickResets(); }, [&st] (bool v) { st.setDoubleClickResets (! v); });
        row ("Mouse wheel param control", oo, [] { return true; }, {});
        row ("Keyboard shortcuts", oo, [] { return true; }, {});
        row ("Default waveform view", { "2D", "3D" }, [] { return false; }, {});
        row ("Modulation halos on knobs", hs, [&st] { return st.showModHalos(); }, [&st] (bool v) { st.setShowModHalos (v); });
        header ("MPE");
        row ("MPE enabled by default", {}, [] { return false; }, {});
        row ("MPE Pitch Bend (also) maps to Expr X", {}, [] { return false; }, {});
        row ("MPE Expr Y acts bi-directional", {}, [] { return false; }, {});
        header ("GENERAL");
        row ("Limit Mod depth on drop (based on knob value)", {}, [] { return false; }, {});
        row ("Mod Wheel -> WT Pos (when WT Editor is open)", {}, [] { return false; }, {});
        row ("Silence note + FX tails when host transport stops", {}, [] { return false; }, {});
        row ("Load MIDI Map from Presets", {}, [&st] { return st.loadMidiMapFromPreset(); }, [&st] (bool v) { st.setLoadMidiMapFromPreset (v); });
        row ("Preset change resets the Arp", {}, [&st] { return st.presetChangeResetsArp(); }, [&st] (bool v) { st.setPresetChangeResetsArp (v); });
        row ("Use Ultra quality when rendering", {}, [] { return false; }, {});
        row ("Automatically check for updates", {}, [] { return false; }, {});
        ignoreUnused (s);

        author.getText = [&st] { const auto a = st.artistName(); return a.isEmpty() ? String ("(none)") : a; };
        author.onOpen = [this]
        {
            if (ctx.editValue) ctx.editValue (author, proc.settings.artistName(), [this] (const String& t) { proc.settings.setArtistName (t.trim()); author.refresh(); });
        };
        author.fontH = 12.5f;
        author.setTooltip ("Default author for new presets");
        addAndMakeVisible (author);

        // Voice control.
        const auto& gid = ids().global;
        auto field = [this] (int id, const String& cap)
        {
            auto f = std::make_unique<ParamField> (ctx, id, cap);
            f->captionLeft = true;
            f->fontH = 13.0f;
            addAndMakeVisible (*f);
            fields.push_back (std::move (f));
        };
        field (gid.poly, "POLY");
        field (gid.bendUp, "BEND UP");
        field (gid.bendDown, "BEND DOWN");
        field (gid.transpose, "TRANSPOSE");
        steal = std::make_unique<ParamChoice> (ctx, gid.voiceSteal);
        steal->fontH = 12.5f;
        steal->caption = "STEAL";
        steal->captionLeft = true;
        addAndMakeVisible (*steal);
        for (auto [id, l] : { std::pair<int, const char*> { gid.mono, "MONO" }, { gid.legato, "LEGATO" }, { gid.portaAlways, "ALWAYS" }, { gid.portaScaled, "SCALED" } })
        {
            auto t = std::make_unique<ParamToggle> (ctx, id, l);
            t->setFontHeight (11.0f);
            addAndMakeVisible (*t);
            toggles.push_back (std::move (t));
        }
        porta = std::make_unique<ParamKnob> (ctx, gid.porta, "PORTA");
        portaCurve = std::make_unique<ParamKnob> (ctx, gid.portaCurve, "CURVE");
        for (auto* k : { porta.get(), portaCurve.get() }) { k->small = true; addAndMakeVisible (*k); }

        // Quality.
        quality.getText = [this] { const int q = proc.quality(); return String (q >= 4 ? "ULTRA" : q >= 2 ? "HIGH" : "GOOD"); };
        quality.onOpen = [this]
        {
            Menu m;
            const int q = proc.quality();
            auto& pr = proc;
            m.add ("Good", [&pr] { pr.setQuality (1); }, q == 1);
            m.add ("High", [&pr] { pr.setQuality (2); }, q == 2);
            m.add ("Ultra", [&pr] { pr.setQuality (4); }, q == 4);
            showMenu (quality, m);
        };
        quality.fontH = 13.0f;
        quality.setTooltip ("Oversampling: Good 1x, High 2x, Ultra 4x");
        addAndMakeVisible (quality);
        for (auto* b : { &qLock, &tLock, &s1Compat, &noSmooth })
        {
            b->setDimmed (true);
            b->setEnabled (false);
            b->setFontHeight (10.5f);
            b->setTooltip ("Not available in Tincture yet");
            addAndMakeVisible (*b);
        }

        // Tuning.
        concertA = std::make_unique<ParamField> (ctx, gid.concertA, "A =");
        concertA->captionLeft = true;
        concertA->fontH = 13.0f;
        addAndMakeVisible (*concertA);
        tuning = std::make_unique<ParamKnob> (ctx, gid.tuning, "MAIN TUNING");
        tuning->small = true;
        addAndMakeVisible (*tuning);
        tunFile.getText = [] { return String ("(NONE)"); };
        tunFile.onOpen = [this]
        {
            Menu m;
            m.add ("Load Tuning...", {}, false, false);
            m.add ("Clear Tuning", {}, false, false);
            m.separator();
            m.toggle ("Enable MTS-ESP", true, {}, false);
            m.toggle ("MTS-ESP Note-On Only", false, {}, false);
            showMenu (tunFile, m);
        };
        tunFile.fontH = 12.5f;
        tunFile.caption = "TUN FILE";
        tunFile.captionLeft = true;
        tunFile.setTooltip ("Tuning files (MTS-ESP / .tun are not available yet)");
        addAndMakeVisible (tunFile);
    }
    ~GlobalPage() override { ctx.remove (this); }

    void refresh() override
    {
        if (! fxui::panelVisible (*this)) return;
        quality.refresh();
        tunFile.refresh();
        if (steal) steal->refresh();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (colours::panel);
        drawSection (g, prefArea.toFloat(), "PREFERENCES");
        drawSection (g, voiceArea.toFloat(), "VOICE CONTROL");
        drawSection (g, qualityArea.toFloat(), "QUALITY");
        drawSection (g, tuningArea.toFloat(), "TUNING");
        drawSection (g, buildArea.toFloat(), "BUILD");
        for (auto& [t, i] : headers)
        {
            const auto y = (float) (i < (int) rows.size() ? rows[(size_t) i]->getY() : 0) - 24.0f;
            drawText (g, t, { (float) prefArea.getX() + 18, y, 300, 18 }, 12.0f, colours::brand, true, Justification::centredLeft);
            g.setColour (colours::inkFaint);
            g.drawHorizontalLine ((int) y + 19, (float) prefArea.getX() + 18, (float) prefArea.getRight() - 18);
        }
        drawText (g, "DEFAULT AUTHOR", { (float) prefArea.getX() + 18, (float) author.getY(), 160, (float) author.getHeight() }, 12.0f, colours::ink, false, Justification::centredLeft);
        // Voice sequencer (no backing data): drawn greyed.
        const auto vs = voiceSeqArea.toFloat();
        g.setColour (colours::inkFaint);
        g.drawRect (vs, 1.0f);
        drawText (g, "VOICE SEQUENCE (per-voice PAN / DETUNE / CUTOFF / ENVS / MOD 1-2), RANDOM and SCALING", vs.withHeight (24.0f).translated (0, 8), 11.0f, colours::inkFaint, true);
        const float cw = (vs.getWidth() - 40.0f) / 8.0f;
        for (int i = 0; i < 8; ++i)
        {
            const auto col = Rectangle<float> (vs.getX() + 20 + (float) i * cw, vs.getY() + 64, cw - 6, vs.getHeight() - 104);
            g.setColour (colours::socket.withAlpha (0.6f));
            g.fillRect (col);
            drawText (g, String (i + 1), col.withHeight (16.0f).translated (0, -20), 11.0f, colours::inkFaint, true);
        }
        drawText (g, "Not available in Tincture yet", vs.withTop (vs.getBottom() - 30), 11.0f, colours::inkFaint, false);
        // Quality / tuning captions.
        drawText (g, "OVERSAMPLING", { (float) quality.getX(), (float) quality.getY() - 18, 200, 14 }, 11.0f, colours::inkDim, true, Justification::centredLeft);
        // Build box.
        const auto b = buildArea.toFloat().reduced (18.0f, 18.0f);
        drawLcdBox (g, b);
        drawText (g, String ("TINCTURE ") + JucePlugin_VersionString, b.withHeight (b.getHeight() * 0.5f), 14.0f, colours::lcd, true);
        drawText (g, String (__DATE__).toUpperCase(), b.withTrimmedTop (b.getHeight() * 0.5f), 12.0f, colours::lcdDim, true);
    }

    void resized() override
    {
        prefArea = { 16, 18, 520, 766 };
        voiceArea = { 556, 18, 480, 766 };
        qualityArea = { 1056, 18, 328, 220 };
        tuningArea = { 1056, 258, 328, 300 };
        buildArea = { 1056, 578, 328, 206 };

        int y = prefArea.getY() + 38;
        size_t h = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (h < headers.size() && headers[h].second == (int) i) { y += 30; ++h; }
            rows[i]->setBounds (prefArea.getX() + 18, y, prefArea.getWidth() - 36, 26);
            y += 30;
        }
        author.setBounds (prefArea.getX() + 190, y + 16, prefArea.getWidth() - 208, 26);

        const int vx = voiceArea.getX() + 22;
        int vy = voiceArea.getY() + 30;
        fields[0]->setBounds (vx, vy, 160, 28);
        steal->setBounds (vx + 190, vy, 246, 28);
        vy += 40;
        for (size_t i = 0; i < 2; ++i) toggles[i]->setBounds (vx + (int) i * 86, vy, 80, 26);
        vy += 44;
        porta->setBounds (vx, vy, 64, 64);
        portaCurve->setBounds (vx + 80, vy, 64, 64);
        toggles[2]->setBounds (vx + 170, vy + 6, 80, 24);
        toggles[3]->setBounds (vx + 260, vy + 6, 80, 24);
        vy += 84;
        fields[1]->setBounds (vx, vy, 200, 28);
        fields[2]->setBounds (vx + 230, vy, 206, 28);
        vy += 40;
        fields[3]->setBounds (vx, vy, 200, 28);
        vy += 56;
        voiceSeqArea = { vx, vy, voiceArea.getWidth() - 44, voiceArea.getBottom() - vy - 22 };

        const int qx = qualityArea.getX() + 22;
        qLock.setBounds (qx, qualityArea.getY() + 46, 54, 30);
        quality.setBounds (qx + 62, qualityArea.getY() + 46, 220, 30);
        s1Compat.setBounds (qx, qualityArea.getY() + 100, 284, 28);
        noSmooth.setBounds (qx, qualityArea.getY() + 140, 284, 28);

        const int tx = tuningArea.getX() + 22;
        tLock.setBounds (tx, tuningArea.getY() + 30, 54, 30);
        concertA->setBounds (tx + 62, tuningArea.getY() + 30, 220, 30);
        tuning->setBounds (tx, tuningArea.getY() + 80, 84, 84);
        tunFile.setBounds (tx, tuningArea.getY() + 190, 284, 30);
    }

private:
    PanelContext& ctx;
    TinctureProcessor& proc;
    std::vector<std::unique_ptr<PrefRow>> rows;
    std::vector<std::pair<String, int>> headers;
    ChoiceDisplay author, quality, tunFile;
    std::vector<std::unique_ptr<ParamField>> fields;
    std::vector<std::unique_ptr<ParamToggle>> toggles;
    std::unique_ptr<ParamChoice> steal;
    std::unique_ptr<ParamKnob> porta, portaCurve, tuning;
    std::unique_ptr<ParamField> concertA;
    PanelButton qLock { "LOCK" }, tLock { "LOCK" }, s1Compat { "S1 COMPATIBILITY MODE" }, noSmooth { "DISABLE SMOOTHING" };
    Rectangle<int> prefArea, voiceArea, qualityArea, tuningArea, buildArea, voiceSeqArea;
};
} // namespace

std::unique_ptr<juce::Component> createGlobalPage (PanelContext& ctx)
{
    return std::make_unique<GlobalPage> (ctx);
}
} // namespace tincture::ui
