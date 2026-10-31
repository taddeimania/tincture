#include "Bind.h"
#include "../PluginProcessor.h"
#include "../Engine/Fx/FxLayout.h"
#include "../Engine/Fx/FxTypes.h"

namespace tincture::ui
{
using namespace juce;

// ===========================================================================
// PanelContext
// ===========================================================================
Colour PanelContext::sourceColour (ModSource s)
{
    const int i = (int) s;
    if (i <= (int) ModSource::Env4) return Colour (0xffffae42);                     // envelopes: amber
    if (i <= (int) ModSource::Lfo10) return Colour (0xff5cc8f2);                    // LFOs: blue
    if (i >= (int) ModSource::Macro1 && i <= (int) ModSource::Macro8) return Colour (0xffa98dff);
    if (i >= (int) ModSource::OscA && i <= (int) ModSource::Sub) return colours::brand;
    if (s == ModSource::Filter1 || s == ModSource::Filter2) return Colour (0xff7fb8ff);
    return Colour (0xffff7088);                                                     // note / performance
}

int PanelContext::haloSlotFor (int param)
{
    if (auto it = haloOf.find (param); it != haloOf.end()) return it->second;
    auto& t = proc.telemetry();
    for (int s = 0; s < Telemetry::kHalo; ++s)
    {
        bool used = false;
        for (auto& [p, slot] : haloOf) used |= slot == s;
        if (! used)
        {
            haloOf[param] = s;
            t.haloParam[s].store (param);
            return s;
        }
    }
    return -1;
}

void PanelContext::releaseHalo (int param)
{
    if (auto it = haloOf.find (param); it != haloOf.end())
    {
        proc.telemetry().haloParam[it->second].store (-1);
        haloOf.erase (it);
    }
}

float PanelContext::haloValue (int slot) const
{
    return slot < 0 ? -1.0f : proc.telemetry().haloNorm[slot].load();
}

String PanelContext::paramName (int param) const
{
    const auto& info = params()[param];
    if (info.group == "FX")
    {
        // "FX 3 P5" -> "REVERB Size" when unit 3 holds a reverb.
        const auto& I = ids();
        for (int u = 0; u < kNumFxUnits; ++u)
        {
            int slot = -2;
            if (I.fx[u].mix == param) slot = -1;
            for (int k = 0; k < kFxParams; ++k) if (I.fx[u].p[k] == param) slot = k;
            if (slot == -2) continue;
            String name;
            proc.patch().fx.forEach ([&] (const FxModuleSlot& m, const FxPath&)
            {
                if (m.unit != u || name.isNotEmpty()) return;
                const auto& fi = fxInfo (m.type);
                if (slot < 0) { name = String (fi.name) + " Mix"; return; }
                float norm[kFxParams];
                for (int k = 0; k < kFxParams; ++k) norm[k] = proc.params.get (I.fx[u].p[k]);
                name = String (fi.name) + " " + String (fxSlotName (m.type, slot, norm));
            });
            if (name.isNotEmpty()) return name;
            break;
        }
    }
    return String (info.name);
}

PanelContext& contextOf (Component& c)
{
    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
        if (auto* h = dynamic_cast<PanelContext*> (p)) return *h;
    jassertfalse;
    static PanelContext* none = nullptr;
    return *none;
}

// ===========================================================================
// Menus
// ===========================================================================
Menu sourceMenu (const std::function<void (ModSource)>& onPick, ModSource current)
{
    Menu m;
    for (const auto& cat : modSourceTree())
    {
        if (cat.leaf)
        {
            const auto s = cat.sources.front();
            m.add (modSourceInfo (s).menuName, [onPick, s] { onPick (s); }, s == current)
             .withSwatch (PanelContext::sourceColour (s));
            continue;
        }
        Menu sub;
        bool has = false;
        for (auto s : cat.sources)
        {
            sub.add (modSourceInfo (s).menuName, [onPick, s] { onPick (s); }, s == current)
               .withSwatch (PanelContext::sourceColour (s));
            has |= s == current;
        }
        m.sub (cat.name, sub, has);
    }
    return m;
}

Menu destinationMenu (const std::function<void (int)>& onPick, int current)
{
    const auto& reg = params();
    Menu m;
    std::string group;
    Menu sub;
    bool subTicked = false;
    auto flush = [&]
    {
        if (! group.empty() && ! sub.empty()) m.sub (group, sub, subTicked);
        sub = Menu();
        subTicked = false;
    };
    for (int i = 0; i < reg.size(); ++i)
    {
        const auto& p = reg[i];
        if (! p.modulatable()) continue;
        if (p.group != group) { flush(); group = p.group; }
        // Drop the module prefix from the item ("A WT Pos" -> "WT Pos").
        String name (p.name);
        sub.add (name, [onPick, i] { onPick (i); }, i == current);
        subTicked |= i == current;
    }
    flush();
    m.searchable();
    return m;
}

Menu choiceMenu (PanelContext& ctx, int param)
{
    auto& proc = ctx.proc;
    const auto& info = params()[param];
    const int cur = proc.params.geti (param);
    Menu m;
    for (int i = 0; i < (int) info.choices.size(); ++i)
        m.add (info.choices[(size_t) i], [&proc, param, i] { proc.setWithUndo (param, (float) i); }, i == cur);
    return m;
}

Menu controlMenu (PanelContext& ctx, int param, const std::function<void (Menu&)>& extra)
{
    auto& proc = ctx.proc;
    const auto& info = params()[param];
    Menu m;
    m.header (ctx.paramName (param));
    if (extra) { extra (m); m.separator(); }

    if (info.modulatable())
    {
        m.sub ("Mod Source", sourceMenu ([&proc, param] (ModSource s)
        {
            proc.editPatch ("Add Modulation", [s, param] (Patch& p) { p.matrix.assign (s, param); });
        }));
        // Rows on this parameter.
        std::vector<int> rows;
        for (int r = 0; r < (int) proc.patch().matrix.rows.size(); ++r)
            if (proc.patch().matrix.rows[(size_t) r].dest == param) rows.push_back (r);
        auto rowMenu = [&] (const String& verb, std::function<void (Patch&, int)> act)
        {
            if (rows.size() <= 1)
            {
                const int r = rows.empty() ? -1 : rows.front();
                m.add (verb + " Modulator", [&proc, act, r, verb] { proc.editPatch (verb + " Modulator", [act, r] (Patch& p) { act (p, r); }); }, false, r >= 0);
                return;
            }
            Menu sub;
            for (int r : rows)
            {
                const auto& row = proc.patch().matrix.rows[(size_t) r];
                sub.add (modSourceInfo (row.source).menuName, [&proc, act, r, verb] { proc.editPatch (verb + " Modulator", [act, r] (Patch& p) { act (p, r); }); },
                         verb == "Bypass" && row.bypass)
                   .withSwatch (PanelContext::sourceColour (row.source));
            }
            m.sub (verb + " Modulator", sub);
        };
        rowMenu ("Bypass", [] (Patch& p, int r) { if (r >= 0) p.matrix.rows[(size_t) r].bypass = ! p.matrix.rows[(size_t) r].bypass; });
        rowMenu ("Remove", [] (Patch& p, int r) { if (r >= 0) p.matrix.remove (r); });
        m.add ("Remove All Modulators", [&proc, param] { proc.editPatch ("Remove All Modulators", [param] (Patch& p) { p.matrix.removeDest (param); }); },
               false, ! rows.empty());
        m.separator();
    }

    m.add ("Reset Control", [&proc, param] { proc.setWithUndo (param, params()[param].def); });
    if (proc.params.host (param) != nullptr)
    {
        const int cc = proc.ccFor (param);
        if (proc.midiLearnTarget() == param)
            m.add ("Cancel MIDI Learn", [&proc, param] { proc.clearMidiLearn (param); });
        else
            m.add ("MIDI Learn", [&proc, param] { proc.startMidiLearn (param); });
        if (cc >= 0)
            m.add ("Clear MIDI Learn (CC " + String (cc) + ")", [&proc, param] { proc.clearMidiLearn (param); });
    }
    m.toggle ("Lock Parameter", proc.isLocked (param), [&proc, param] { proc.setLocked (param, ! proc.isLocked (param)); });
    return m;
}

var modSourceDragDescription (ModSource s) { return "modsrc:" + String ((int) s); }

ModSource modSourceFromDrag (const var& v)
{
    const auto s = v.toString();
    if (! s.startsWith ("modsrc:")) return ModSource::None;
    const int i = s.fromFirstOccurrenceOf (":", false, false).getIntValue();
    return i >= 0 && i < kNumModSources ? (ModSource) i : ModSource::None;
}

void dropModulation (PanelContext& ctx, ModSource s, int param)
{
    if (s == ModSource::None || ! params()[param].modulatable()) return;
    ctx.proc.editPatch ("Add Modulation", [s, param] (Patch& p)
    {
        const int r = p.matrix.assign (s, param, 1.0f);   // spec §3: a drop starts at 100 %
        (void) r;
    });
}

// ===========================================================================
// Bound
// ===========================================================================
Bound::Bound (PanelContext& c, int p) : ctx (c), paramId (p) { ctx.add (this); }
Bound::~Bound() { ctx.remove (this); }
void Bound::setParam (int p) { paramId = p; shownNorm = -1.0f; refresh(); }

String Bound::tooltipFor() const
{
    const auto& info = params()[paramId];
    return ctx.paramName (paramId) + ": " + String (info.format (ctx.proc.params.get (paramId)));
}

// ===========================================================================
// ParamKnob
// ===========================================================================
ParamKnob::ParamKnob (PanelContext& c, int p, const String& cap) : Bound (c, p)
{
    caption = cap.isNotEmpty() ? cap : String (params()[p].shortName);
    setRange (0.0, 1.0);
    bipolar = params()[p].bipolar();
    format = [this] (double n) { const auto& i = params()[paramId]; return String (i.format (i.fromNorm ((float) n))); };
    setDoubleClickReturnValue (false, 0.0);
    refresh();
}

void ParamKnob::refresh()
{
    auto& proc = ctx.proc;
    const auto& info = params()[paramId];
    const float n = info.toNorm (proc.params.get (paramId));
    bool dirty = false;
    if (! same (n, shownNorm) && ! isMouseButtonDown())
    {
        syncing = true;
        setValue (n, dontSendNotification);
        syncing = false;
        shownNorm = n;
        dirty = true;
    }
    // Modulation rings from the matrix.
    std::vector<Ring> now;
    for (const auto& r : proc.patch().matrix.rows)
    {
        if (r.dest != paramId || r.source == ModSource::None) continue;
        const float a = r.amount * r.output;
        float from = n, to = n + a;
        if (r.bipolar) { from = n - std::abs (a) * 0.5f; to = n + std::abs (a) * 0.5f; }
        now.push_back ({ jlimit (0.0f, 1.0f, from), jlimit (0.0f, 1.0f, to), PanelContext::sourceColour (r.source), r.bypass });
    }
    const bool changed = now.size() != rings.size()
        || ! std::equal (now.begin(), now.end(), rings.begin(), [] (const Ring& a, const Ring& b) { return same (a.from, b.from) && same (a.to, b.to) && a.bypassed == b.bypassed; });
    if (changed) { rings = now; dirty = true; }
    if (! rings.empty() && showHalo)
    {
        if (haloSlot < 0) haloSlot = ctx.haloSlotFor (paramId);
        dirty = true;   // the live position moves
    }
    else if (haloSlot >= 0)
    {
        ctx.releaseHalo (paramId);
        haloSlot = -1;
    }
    if (dirty) repaint();
}

void ParamKnob::paint (Graphics& g)
{
    Knob::paint (g);
    auto area = getLocalBounds().toFloat();
    const float capH = caption.isNotEmpty() ? (big ? 30.0f : (small ? 14.0f : 18.0f)) : 0.0f;
    const auto dial = area.withTrimmedBottom (capH);
    const float side = jmin (dial.getWidth(), dial.getHeight());
    const auto c = dial.getCentre();
    const float r = side * 0.5f - 1.5f;
    const auto rp = getRotaryParameters();
    auto angle = [&] (float t) { return rp.startAngleRadians + t * (rp.endAngleRadians - rp.startAngleRadians); };

    float ringR = r;
    for (const auto& ring : rings)
    {
        Path p;
        p.addCentredArc (c.x, c.y, ringR, ringR, 0.0f, angle (jmin (ring.from, ring.to)), angle (jmax (ring.from, ring.to)), true);
        g.setColour (ring.bypassed ? ring.colour.withAlpha (0.3f) : ring.colour);
        g.strokePath (p, PathStrokeType (2.0f));
        ringR -= 2.5f;
    }
    if (haloSlot >= 0 && ! rings.empty())
    {
        const float live = ctx.haloValue (haloSlot);
        if (live >= 0.0f)
        {
            const auto pt = c.getPointOnCircumference (r + 1.0f, angle (live));
            g.setColour (colours::ink);
            g.fillEllipse (Rectangle<float> (5.0f, 5.0f).withCentre (pt));
        }
    }
    if (dropHover)
    {
        g.setColour (colours::brand.withAlpha (0.5f));
        g.drawEllipse (Rectangle<float> (side, side).withCentre (c), 2.0f);
    }
}

void ParamKnob::valueChanged()
{
    if (syncing) return;
    const auto& info = params()[paramId];
    ctx.proc.set (paramId, info.fromNorm ((float) getValue()));
    shownNorm = (float) getValue();
    if (ctx.setStatus) ctx.setStatus (tooltipFor());
}

void ParamKnob::startedDragging() { ctx.proc.beginGesture (paramId); }
void ParamKnob::stoppedDragging() { ctx.proc.endGesture (paramId); }

void ParamKnob::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showContextMenu (*this, e.getPosition(), controlMenu (ctx, paramId, extraMenu));
        return;
    }
    if (e.mods.isCommandDown())
    {
        ctx.proc.setWithUndo (paramId, params()[paramId].def);
        refresh();
        return;
    }
    Knob::mouseDown (e);
}

void ParamKnob::mouseDoubleClick (const MouseEvent&)
{
    auto& proc = ctx.proc;
    if (proc.settings.doubleClickResets()) { proc.setWithUndo (paramId, params()[paramId].def); return; }
    if (ctx.editValue)
    {
        const int p = paramId;
        ctx.editValue (*this, String (params()[p].format (proc.params.get (p), false)), [&proc, p] (const String& t)
        {
            float v;
            if (params()[p].parse (t.toStdString(), v)) proc.setWithUndo (p, v);
        });
    }
}

void ParamKnob::mouseEnter (const MouseEvent& e)
{
    Knob::mouseEnter (e);
    if (ctx.setStatus) ctx.setStatus (tooltipFor());
}

bool ParamKnob::isInterestedInDragSource (const SourceDetails& d)
{
    return modSourceFromDrag (d.description) != ModSource::None && params()[paramId].modulatable();
}

void ParamKnob::itemDropped (const SourceDetails& d)
{
    dropHover = false;
    dropModulation (ctx, modSourceFromDrag (d.description), paramId);
    repaint();
}

// ===========================================================================
// ParamFader
// ===========================================================================
ParamFader::ParamFader (PanelContext& c, int p, bool vertical, const String& cap) : Fader (vertical), Bound (c, p)
{
    caption = cap;
    setRange (0.0, 1.0);
    bipolar = params()[p].bipolar();
    format = [this] (double n) { const auto& i = params()[paramId]; return String (i.format (i.fromNorm ((float) n))); };
    refresh();
}

void ParamFader::refresh()
{
    const float n = params()[paramId].toNorm (ctx.proc.params.get (paramId));
    if (! same (n, shownNorm) && ! isMouseButtonDown())
    {
        syncing = true;
        setValue (n, dontSendNotification);
        syncing = false;
        shownNorm = n;
        repaint();
    }
}

void ParamFader::valueChanged()
{
    if (syncing) return;
    ctx.proc.set (paramId, params()[paramId].fromNorm ((float) getValue()));
    shownNorm = (float) getValue();
    if (ctx.setStatus) ctx.setStatus (tooltipFor());
}
void ParamFader::startedDragging() { ctx.proc.beginGesture (paramId); }
void ParamFader::stoppedDragging() { ctx.proc.endGesture (paramId); }

void ParamFader::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), controlMenu (ctx, paramId, extraMenu)); return; }
    if (e.mods.isCommandDown()) { ctx.proc.setWithUndo (paramId, params()[paramId].def); refresh(); return; }
    Fader::mouseDown (e);
}

void ParamFader::mouseDoubleClick (const MouseEvent&)
{
    auto& proc = ctx.proc;
    const int p = paramId;
    if (ctx.editValue)
        ctx.editValue (*this, String (params()[p].format (proc.params.get (p), false)), [&proc, p] (const String& t)
        {
            float v;
            if (params()[p].parse (t.toStdString(), v)) proc.setWithUndo (p, v);
        });
}

bool ParamFader::isInterestedInDragSource (const SourceDetails& d) { return modSourceFromDrag (d.description) != ModSource::None; }
void ParamFader::itemDropped (const SourceDetails& d) { dropModulation (ctx, modSourceFromDrag (d.description), paramId); }

// ===========================================================================
// ParamField
// ===========================================================================
ParamField::ParamField (PanelContext& c, int p, const String& cap) : Bound (c, p), caption (cap)
{
    setRepaintsOnMouseActivity (false);
    refresh();
}

void ParamField::refresh()
{
    const float n = params()[paramId].toNorm (ctx.proc.params.get (paramId));
    if (! same (n, shownNorm)) { shownNorm = n; repaint(); setTooltip (tooltipFor()); }
}

void ParamField::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const auto& info = params()[paramId];
    if (caption.isNotEmpty())
    {
        if (captionLeft)
        {
            const float w = jmin (r.getWidth() * 0.5f, (float) GlyphArrangement::getStringWidth (panelFont (10.5f), caption) + 8.0f);
            drawText (g, caption, r.removeFromLeft (w), 10.5f, colours::inkDim, true, Justification::centredLeft);
        }
        else
            drawText (g, caption, r.removeFromBottom (13.0f), 10.5f, colours::ink, true);
    }
    r = r.reduced (0.5f);
    drawLcdBox (g, r);
    const float v = ctx.proc.params.get (paramId);
    const String text = format ? format (v) : String (info.format (v));
    drawText (g, text, r.reduced (3.0f, 0.0f), fontH, isEnabled() ? colours::lcd : colours::lcdDim, true);
}

void ParamField::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), controlMenu (ctx, paramId, extraMenu)); return; }
    if (e.mods.isCommandDown()) { ctx.proc.setWithUndo (paramId, params()[paramId].def); return; }
    dragStartNorm = params()[paramId].toNorm (ctx.proc.params.get (paramId));
    dragging = false;
}

void ParamField::mouseDrag (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    const auto& info = params()[paramId];
    if (! dragging) { dragging = true; ctx.proc.beginGesture (paramId); }
    const float dy = (float) -e.getDistanceFromDragStartY() + (float) e.getDistanceFromDragStartX() * 0.5f;
    if (info.isDiscrete())
    {
        const float steps = std::round (dy / (e.mods.isShiftDown() ? pixelsPerStep * 3.0f : pixelsPerStep));
        const float start = info.fromNorm (dragStartNorm);
        ctx.proc.set (paramId, start + steps);
    }
    else
    {
        const float sens = e.mods.isShiftDown() ? 1500.0f : 250.0f;
        ctx.proc.set (paramId, info.fromNorm (jlimit (0.0f, 1.0f, dragStartNorm + dy / sens)));
    }
    refresh();
}

void ParamField::mouseUp (const MouseEvent&)
{
    if (dragging) ctx.proc.endGesture (paramId);
    dragging = false;
}

void ParamField::mouseDoubleClick (const MouseEvent&)
{
    auto& proc = ctx.proc;
    const int p = paramId;
    if (ctx.editValue)
        ctx.editValue (*this, String (params()[p].format (proc.params.get (p), false)), [&proc, p] (const String& t)
        {
            float v;
            if (params()[p].parse (t.toStdString(), v)) proc.setWithUndo (p, v);
        });
}

void ParamField::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    const auto& info = params()[paramId];
    const float cur = ctx.proc.params.get (paramId);
    if (info.isDiscrete()) ctx.proc.set (paramId, cur + (w.deltaY > 0 ? 1.0f : -1.0f));
    else ctx.proc.set (paramId, info.fromNorm (jlimit (0.0f, 1.0f, info.toNorm (cur) + w.deltaY * 0.05f)));
    refresh();
}

bool ParamField::isInterestedInDragSource (const SourceDetails& d)
{
    return modSourceFromDrag (d.description) != ModSource::None && params()[paramId].modulatable();
}
void ParamField::itemDropped (const SourceDetails& d) { dropModulation (ctx, modSourceFromDrag (d.description), paramId); }

// ===========================================================================
// ParamToggle
// ===========================================================================
ParamToggle::ParamToggle (PanelContext& c, int p, const String& text) : PanelButton (text), Bound (c, p)
{
    onClick = [this] (const ModifierKeys& m)
    {
        if (m.isPopupMenu()) { showContextMenu (*this, getMouseXYRelative(), controlMenu (ctx, paramId)); return; }
        ctx.proc.setWithUndo (paramId, ctx.proc.params.on (paramId) ? 0.0f : 1.0f);
        refresh();
    };
    refresh();
}

void ParamToggle::refresh()
{
    setLit (ctx.proc.params.on (paramId));
}

// ===========================================================================
// ParamChoice
// ===========================================================================
ParamChoice::ParamChoice (PanelContext& c, int p) : Bound (c, p)
{
    getText = [this]
    {
        const int i = ctx.proc.params.geti (paramId);
        if (label) return label (i);
        return String (params()[paramId].format ((float) i)).toUpperCase();
    };
    refresh();
}

void ParamChoice::refresh() { ChoiceDisplay::refresh(); }

void ParamChoice::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), controlMenu (ctx, paramId)); return; }
    showMenu (*this, buildMenu ? buildMenu() : choiceMenu (ctx, paramId));
}

void ParamChoice::step (int d)
{
    const auto& info = params()[paramId];
    const int n = (int) info.max + 1;
    const int cur = ctx.proc.params.geti (paramId);
    ctx.proc.setWithUndo (paramId, (float) ((cur + d + n) % n));
    refresh();
}

void ParamChoice::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    step (w.deltaY > 0 ? -1 : 1);
}

// ===========================================================================
// ModSourceTab
// ===========================================================================
ModSourceTab::ModSourceTab (PanelContext& c, ModSource s, const String& label) : ctx (c), src (s), text (label)
{
    setTooltip ("Drag onto a control to modulate it with " + String (modSourceInfo (s).menuName));
}

void ModSourceTab::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (selected ? colours::cream : colours::black);
    g.fillRect (r);
    g.setColour (colours::ink.withAlpha (0.8f));
    g.drawRect (r, 1.0f);
    // A small "drag me" grip in the source colour.
    const auto grip = r.withWidth (5.0f).translated (2.0f, 0.0f).reduced (0.0f, 4.0f);
    g.setColour (PanelContext::sourceColour (src));
    g.fillRect (grip);
    drawText (g, text, r.withTrimmedLeft (8.0f), 12.0f, selected ? colours::black : colours::ink, true);
}

void ModSourceTab::mouseDown (const MouseEvent& e)
{
    dragStarted = false;
    if (e.mods.isPopupMenu())
    {
        Menu m;
        m.header (modSourceInfo (src).menuName);
        if (extraMenu) { extraMenu (m); m.separator(); }
        auto& proc = ctx.proc;
        const auto s = src;
        bool any = false, allBypassed = true;
        for (const auto& r : proc.patch().matrix.rows)
            if (r.source == s) { any = true; allBypassed &= r.bypass; }
        m.toggle ("Bypass All Destinations", any && allBypassed, [&proc, s, allBypassed]
        {
            proc.editPatch ("Bypass Modulator", [s, allBypassed] (Patch& p) { p.matrix.setSourceBypass (s, ! allBypassed); });
        }, any);
        m.add ("Remove All Destinations", [&proc, s] { proc.editPatch ("Remove Modulations", [s] (Patch& p) { p.matrix.removeSource (s); }); }, false, any);
        showContextMenu (*this, e.getPosition(), m);
    }
}

void ModSourceTab::mouseDrag (const MouseEvent& e)
{
    if (e.mods.isPopupMenu() || dragStarted || e.getDistanceFromDragStart() < 4) return;
    if (auto* dnd = DragAndDropContainer::findParentDragContainerFor (this))
    {
        dragStarted = true;
        dnd->startDragging (modSourceDragDescription (src), this);
    }
}

void ModSourceTab::mouseUp (const MouseEvent& e)
{
    if (! dragStarted && ! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()) && onClick) onClick();
    dragStarted = false;
}
} // namespace tincture::ui
