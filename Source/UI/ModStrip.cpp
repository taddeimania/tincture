// ModStrip.cpp — the OSC page's modulation strip: source tiles, MACROS,
// ENV 1-4, VELO / NOTE, VOICING / PORTAMENTO and the strip itself. The LFO
// section is in ModStripLfo.cpp.
#include "ModStrip.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
// Host tempo from the engine (120 when the host gives none).
double hostBpm (TinctureProcessor& p) { const double b = p.telemetry().bpm.load(); return b > 1.0 ? b : 120.0; }

const Colour kEnvColour = PanelContext::sourceColour (ModSource::Env1);

void setUi (TinctureProcessor& p, const char* key, const var& v) { p.uiState.setProperty (key, v, nullptr); }
var getUi (TinctureProcessor& p, const char* key, const var& def) { return p.uiState.getProperty (key, def); }

void appendSourceItems (PanelContext& ctx, ModSource s, Menu& m)
{
    auto& proc = ctx.proc;
    const auto& rows = proc.patch().matrix.rows;
    bool any = false, allBypassed = true;
    for (int r = 0; r < (int) rows.size(); ++r)
    {
        const auto& row = rows[(size_t) r];
        if (row.source != s || row.dest < 0) continue;
        any = true;
        allBypassed &= row.bypass;
        m.toggle ("Bypass: " + String (params()[row.dest].name), row.bypass, [&proc, r, s]
        {
            proc.editPatch ("Bypass Modulator", [r, s] (Patch& p)
            {
                if (isPositiveAndBelow (r, (int) p.matrix.rows.size()) && p.matrix.rows[(size_t) r].source == s)
                    p.matrix.rows[(size_t) r].bypass = ! p.matrix.rows[(size_t) r].bypass;
            });
        });
    }
    m.toggle ("Bypass All Destinations", any && allBypassed, [&proc, s, allBypassed]
    {
        proc.editPatch ("Bypass Modulator", [s, allBypassed] (Patch& p) { p.matrix.setSourceBypass (s, ! allBypassed); });
    }, any);
    m.separator();
    m.add ("Remove All Destinations", [&proc, s] { proc.editPatch ("Remove Modulations", [s] (Patch& p) { p.matrix.removeSource (s); }); }, false, any);
}

String knobFormat (int param, double norm)
{
    const auto& i = params()[param];
    return String (i.format (i.fromNorm ((float) norm)));
}
} // namespace

// ===========================================================================
// Helpers
// ===========================================================================
String beatsText (float beats)
{
    if (beats <= 1.0e-5f) return "0";
    struct N { double b; const char* name; };
    static const N straight[] { { 32, "8 bars" }, { 16, "4 bars" }, { 8, "2 bars" }, { 4, "1 bar" }, { 2, "1/2" }, { 1, "1/4" },
                                { 0.5, "1/8" }, { 0.25, "1/16" }, { 0.125, "1/32" }, { 0.0625, "1/64" } };
    for (const auto& n : straight)
        for (int k = 0; k < 3; ++k)
        {
            const double m = k == 0 ? 1.0 : (k == 1 ? 1.5 : 2.0 / 3.0);
            if (std::abs (n.b * m - beats) <= 0.01 * n.b * m)
                return String (n.name) + (k == 1 ? " D" : (k == 2 ? " T" : ""));
        }
    return String (beats, beats < 1.0f ? 3 : 2) + " bt";
}

String sourceTooltip (TinctureProcessor& proc, ModSource s)
{
    String t = String (modSourceInfo (s).menuName) + " Source\n------------\nDestinations:";
    int n = 0;
    for (const auto& r : proc.patch().matrix.rows)
        if (r.source == s && r.dest >= 0) { t << "\n" << params()[r.dest].name; ++n; }
    if (n == 0) t << "\n(none - drag onto a control)";
    return t;
}

Menu sourceTabMenu (PanelContext& ctx, ModSource s)
{
    Menu m;
    appendSourceItems (ctx, s, m);
    return m;
}

void IconButton::paint (Graphics& g)
{
    PanelButton::paint (g);
    if (icon) icon (g, getLocalBounds().toFloat().reduced (3.0f), isLit() ? colours::black : (isEnabled() ? colours::ink : colours::inkDim));
}

// ===========================================================================
// SourceTile
// ===========================================================================
SourceTile::SourceTile (PanelContext& context, ModSource s, const String& l) : ModSourceTab (context, s, l), label (l), c (context)
{
    c.add (this);
    refresh();
}

SourceTile::~SourceTile() { c.remove (this); }

void SourceTile::refresh()
{
    const int n = (int) c.proc.patch().matrix.rowsForSource (source()).size();
    const auto tip = sourceTooltip (c.proc, source());
    if (tip != getTooltip()) setTooltip (tip);
    if (n != count) { count = n; repaint(); }
}

void SourceTile::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const auto col = PanelContext::sourceColour (source());
    if (numberTile)
    {
        g.setColour (count > 0 ? col : colours::black);
        g.fillRect (r);
        g.setColour (hover || dropHover ? colours::ink : col);
        g.drawRect (r, 1.2f);
        drawText (g, label, r, 11.5f, count > 0 ? colours::black : colours::ink, true);
        return;
    }
    g.setColour (selected ? colours::cream : colours::black);
    g.fillRect (r);
    g.setColour (dropHover ? colours::brand : colours::ink.withAlpha (selected ? 1.0f : 0.7f));
    g.drawRect (r, dropHover ? 2.0f : 1.0f);

    // Grip: a source-coloured bar; a move cross when hovered (the drag handle).
    auto left = r.withWidth (10.0f).translated (2.0f, 0.0f);
    if (hover)
    {
        const auto cc = left.getCentre();
        g.setColour (col);
        g.drawLine (cc.x - 4.0f, cc.y, cc.x + 4.0f, cc.y, 1.5f);
        g.drawLine (cc.x, cc.y - 4.0f, cc.x, cc.y + 4.0f, 1.5f);
    }
    else
    {
        g.setColour (col);
        g.fillRect (left.withWidth (4.0f).reduced (0.0f, 4.0f));
    }

    auto textArea = r.withTrimmedLeft (10.0f);
    if (count > 0)
    {
        // Destination count, small and top-right, in the source colour.
        auto badge = textArea.removeFromRight (9.0f);
        drawText (g, String (count), badge.withTrimmedRight (1.0f).withHeight (12.0f), 10.5f, selected ? colours::black : col, true, Justification::centredRight);
    }
    drawText (g, label, textArea, 11.5f, selected ? colours::black : colours::ink, true);
}

Menu SourceTile::buildMenu()
{
    Menu m;
    if (menuExtras) menuExtras (m);
    appendSourceItems (c, source(), m);
    return m;
}

void SourceTile::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showContextMenu (*this, e.getPosition(), buildMenu());
        return;
    }
    ModSourceTab::mouseDown (e);
}

bool SourceTile::isInterestedInDragSource (const SourceDetails& d)
{
    const auto s = modSourceFromDrag (d.description);
    return onTileDrop != nullptr && s != ModSource::None && s != source();
}

void SourceTile::itemDropped (const SourceDetails& d)
{
    dropHover = false;
    repaint();
    if (onTileDrop) onTileDrop (modSourceFromDrag (d.description), ModifierKeys::currentModifiers.isAltDown());
}

// ===========================================================================
// MACROS
// ===========================================================================
String macroName (const Patch& p, int m)
{
    const String n (p.macroNames[(size_t) m]);
    return n.isNotEmpty() ? n : "MACRO " + String (m + 1);
}

MacroPanel::MacroPanel (PanelContext& c) : ctx (c)
{
    ctx.add (this);
    for (int m = 0; m < kNumMacros; ++m)
    {
        knobs[m] = std::make_unique<ParamKnob> (ctx, ids().macro[m], macroName (ctx.proc.patch(), m));
        knobs[m]->small = true;
        knobs[m]->accent = PanelContext::sourceColour (macroSource (m));
        knobs[m]->extraMenu = [this, m] (Menu& menu) { menu.add ("Rename...", [this, m] { rename (m); }); };
        addAndMakeVisible (*knobs[m]);
        tiles[m] = std::make_unique<SourceTile> (ctx, macroSource (m), String (m + 1));
        tiles[m]->numberTile = true;
        tiles[m]->menuExtras = [this, m] (Menu& menu) { menu.add ("Rename...", [this, m] { rename (m); }); menu.separator(); };
        const auto self = macroSource (m);
        auto& proc = ctx.proc;
        // Drop one macro tile onto another to swap their assignments (§8).
        tiles[m]->onTileDrop = [&proc, self] (ModSource other, bool)
        {
            const int o = (int) other;
            if (o < (int) ModSource::Macro1 || o > (int) ModSource::Macro8) return;
            proc.editPatch ("Swap Macros", [self, other] (Patch& p)
            {
                for (auto& r : p.matrix.rows)
                {
                    if (r.source == self) r.source = other;
                    else if (r.source == other) r.source = self;
                    if (r.aux == self) r.aux = other;
                    else if (r.aux == other) r.aux = self;
                }
            });
        };
        addAndMakeVisible (*tiles[m]);
    }
}

MacroPanel::~MacroPanel() { ctx.remove (this); }

void MacroPanel::rename (int m)
{
    if (! ctx.editValue) return;
    auto& proc = ctx.proc;
    ctx.editValue (*knobs[m], macroName (proc.patch(), m), [&proc, m] (const String& text)
    {
        auto t = text.trim().substring (0, 10);
        if (t == "MACRO " + String (m + 1)) t = {};
        proc.editPatch ("Rename Macro", [m, t] (Patch& p) { p.macroNames[(size_t) m] = t.toStdString(); });
    });
}

void MacroPanel::refresh()
{
    for (int m = 0; m < kNumMacros; ++m)
    {
        const auto n = macroName (ctx.proc.patch(), m);
        if (knobs[m]->caption != n) { knobs[m]->caption = n; knobs[m]->repaint(); }
    }
}

void MacroPanel::resized()
{
    for (int m = 0; m < kNumMacros; ++m)
    {
        const int col = m / 4, row = m % 4;
        const int x = 8 + col * 70, y = 14 + row * 51;
        tiles[m]->setBounds (x, y + 4, 18, 18);
        knobs[m]->setBounds (x + 18, y, 50, 50);
    }
}

// ===========================================================================
// ENV graph
// ===========================================================================
EnvGraph::EnvGraph (PanelContext& c) : ctx (c)
{
    ctx.add (this);
    locked = (bool) getUi (ctx.proc, "envLocked", true);
    zoomSeconds = (float) (double) getUi (ctx.proc, "envZoom", 4.0);
    gridBeats = (bool) getUi (ctx.proc, "envGridBeats", false);
}

EnvGraph::~EnvGraph() { ctx.remove (this); }

float EnvGraph::toSeconds (float plain) const
{
    return ctx.proc.params.on (ids().env[env].bpm) ? (float) envBeatsToSeconds (plain, hostBpm (ctx.proc)) : plain;
}

float EnvGraph::fromSeconds (float s) const
{
    return ctx.proc.params.on (ids().env[env].bpm) ? (float) (s * hostBpm (ctx.proc) / 60.0) : s;
}

EnvSettings EnvGraph::settingsOf (int e) const
{
    const auto& id = ids().env[e];
    auto& P = ctx.proc.params;
    const bool bpm = P.on (id.bpm);
    auto t = [&] (int p) { return bpm ? (float) envBeatsToSeconds (P.get (p), hostBpm (ctx.proc)) : P.get (p); };
    EnvSettings s;
    s.attack = t (id.attack);
    s.hold = t (id.hold);
    s.decay = t (id.decay);
    s.release = t (id.release);
    s.sustain = envSustainGain (P.get (id.sustain));
    s.attackCurve = P.get (id.attackCurve);
    s.decayCurve = P.get (id.decayCurve);
    s.releaseCurve = P.get (id.releaseCurve);
    return s;
}

float EnvGraph::viewLength() const
{
    if (dragPoint >= 0 || dragHandle >= 0) return dragView;
    if (! locked) return jmax (0.005f, zoomSeconds);
    return jmax (0.01f, envShapeLength (settingsOf (env)) * 1.04f);
}

Point<float> EnvGraph::toScreen (float t, float level) const
{
    const auto r = plot();
    return { r.getX() + t / viewLength() * r.getWidth(), r.getBottom() - level * r.getHeight() };
}

std::array<Point<float>, 5> EnvGraph::points() const
{
    const auto s = settingsOf (env);
    const float tA = s.attack, tH = tA + s.hold, tD = tH + s.decay, tR = tD + s.release;
    return { toScreen (0, 0), toScreen (tA, 1), toScreen (tH, 1), toScreen (tD, s.sustain), toScreen (tR, 0) };
}

Point<float> EnvGraph::handle (int seg) const
{
    const auto s = settingsOf (env);
    const float tA = s.attack, tH = tA + s.hold, tD = tH + s.decay;
    const float t = seg == 0 ? tA * 0.5f : seg == 1 ? tH + s.decay * 0.5f : tD + s.release * 0.5f;
    return toScreen (t, envShapeAt (s, t));
}

void EnvGraph::refresh()
{
    String key;
    for (int e : { 0, env })
    {
        const auto& id = ids().env[e];
        for (int p : { id.attack, id.hold, id.decay, id.sustain, id.release, id.attackCurve, id.decayCurve, id.releaseCurve, id.bpm })
            key << String (ctx.proc.params.get (p), 5) << ",";
    }
    key << hostBpm (ctx.proc) << (int) locked << zoomSeconds << (int) gridBeats << env << getWidth();
    const auto& t = ctx.proc.telemetry();
    const float live = t.activeVoices.load() > 0 && t.envStage[env].load() != 0 ? t.env[env].load() + (float) t.envStage[env].load() * 10.0f + t.envProgress[env].load() : -1.0f;
    if (key != shownKey || ! same (live, shownLive)) { shownKey = key; shownLive = live; repaint(); }
}

void EnvGraph::paint (Graphics& g)
{
    drawLcdBox (g, getLocalBounds().toFloat());
    const auto r = plot();
    const float len = viewLength();

    // Ruler / grid: seconds, or beats at the assumed tempo.
    {
        const float beat = (float) (60.0 / hostBpm (ctx.proc));
        float step;
        if (gridBeats)
        {
            step = beat / 4.0f;
            while (len / step > 8.0f) step *= 2.0f;
        }
        else
        {
            static const float steps[] { 0.001f, 0.002f, 0.005f, 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 20, 50 };
            step = 50.0f;
            for (float s : steps) if (len / s <= 7.0f) { step = s; break; }
        }
        for (int i = 1; (float) i * step < len; ++i)
        {
            const float x = r.getX() + (float) i * step / len * r.getWidth();
            g.setColour (colours::grid);
            g.drawVerticalLine (roundToInt (x), r.getY(), r.getBottom());
            const float v = (float) i * step;
            String lab;
            if (gridBeats) lab = beatsText (v / beat);
            else lab = v < 1.0f ? String (roundToInt (v * 1000.0f)) + " ms" : String (v, v < 10.0f && std::abs (v - std::round (v)) > 0.01f ? 1 : 0) + " s";
            drawText (g, lab, { x - 22.0f, r.getBottom() + 1.0f, 44.0f, 11.0f }, 10.5f, colours::inkDim, false);
        }
        g.setColour (colours::grid);
        g.drawHorizontalLine (roundToInt (r.getY()), r.getX(), r.getRight());
        g.drawHorizontalLine (roundToInt (r.getBottom()), r.getX(), r.getRight());
    }

    Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (r.expanded (5.0f, 4.0f).toNearestInt());

    auto shapePath = [&] (const EnvSettings& s)
    {
        std::vector<float> xy;
        envShapePolyline (s, jlimit (32, 400, (int) r.getWidth()), xy);
        Path p;
        for (size_t i = 0; i + 1 < xy.size(); i += 2)
        {
            const auto q = toScreen (xy[i], xy[i + 1]);
            if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
        }
        return p;
    };

    if (env > 0)
    {
        g.setColour (colours::inkFaint);
        g.strokePath (shapePath (settingsOf (0)), PathStrokeType (1.2f));
    }
    const auto s = settingsOf (env);
    auto path = shapePath (s);
    auto fillP = path;
    fillP.lineTo (toScreen (envShapeLength (s), 0));
    fillP.lineTo (toScreen (0, 0));
    fillP.closeSubPath();
    g.setColour (kEnvColour.withAlpha (0.14f));
    g.fillPath (fillP);
    g.setColour (kEnvColour);
    g.strokePath (path, PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded));

    for (int seg = 0; seg < 3; ++seg)
    {
        const auto h = handle (seg);
        g.setColour (kEnvColour.withAlpha (0.85f));
        g.drawEllipse (Rectangle<float> (6.4f, 6.4f).withCentre (h), 1.2f);
    }
    const auto pts = points();
    for (int i = 1; i < 5; ++i)
    {
        const auto box = Rectangle<float> (8.0f, 8.0f).withCentre (pts[(size_t) i]);
        g.setColour (i == dragPoint ? colours::cream : colours::black);
        g.fillEllipse (box);
        g.setColour (kEnvColour);
        g.drawEllipse (box, 1.4f);
    }

    // Live position of the newest voice.
    const auto& t = ctx.proc.telemetry();
    const int stage = t.envStage[env].load();
    if (t.activeVoices.load() > 0 && stage != 0)
    {
        // Progress through the stage (0..1) placed on this graph's own time axis.
        const float pr = clamp01 (t.envProgress[env].load());
        const float tA = s.attack, tH = tA + s.hold, tD = tH + s.decay;
        float time = pr * tA;
        if (stage == 2) time = tA + pr * s.hold;
        else if (stage == 3) time = tH + pr * s.decay;
        else if (stage == 4) time = tD;
        else if (stage == 5) time = tD + pr * s.release;
        const auto p = toScreen (time, t.env[env].load());
        g.setColour (colours::ink);
        g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (p));
    }
}

void EnvGraph::beginParams (std::initializer_list<int> ps)
{
    gestureParams.assign (ps);
    for (int p : gestureParams) ctx.proc.beginGesture (p);
}

Menu EnvGraph::graphMenu()
{
    auto& proc = ctx.proc;
    const int p = ids().env[env].legatoInvert;
    Menu m;
    m.toggle ("Legato Inverted", proc.params.on (p), [&proc, p] { proc.setWithUndo (p, proc.params.on (p) ? 0.0f : 1.0f); });
    m.separator();
    Menu grid;
    Component::SafePointer<EnvGraph> self (this);
    grid.add ("Time", [self, &proc] { if (self) { self->gridBeats = false; setUi (proc, "envGridBeats", false); self->repaint(); } }, ! gridBeats);
    grid.add ("Beats", [self, &proc] { if (self) { self->gridBeats = true; setUi (proc, "envGridBeats", true); self->repaint(); } }, gridBeats);
    m.sub ("Grid", grid);
    return m;
}

void EnvGraph::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), graphMenu()); return; }
    const auto pts = points();
    dragPoint = dragHandle = -1;
    float best = 8.0f;
    for (int i = 1; i <= 4; ++i)     // ties (coincident points) go to the earlier one
        if (pts[(size_t) i].getDistanceFrom (e.position) < best) { best = pts[(size_t) i].getDistanceFrom (e.position); dragPoint = i; }
    if (dragPoint < 0)
        for (int s = 0; s < 3; ++s)
            if (handle (s).getDistanceFrom (e.position) < 7.0f) dragHandle = s;
    if (dragPoint < 0 && dragHandle < 0) return;
    dragView = locked ? jmax (0.01f, envShapeLength (settingsOf (env)) * 1.04f) : jmax (0.005f, zoomSeconds);
    const auto& id = ids().env[env];
    if (dragPoint == 1) beginParams ({ id.attack });
    else if (dragPoint == 2) beginParams ({ id.hold });
    else if (dragPoint == 3) beginParams ({ id.decay, id.sustain });
    else if (dragPoint == 4) beginParams ({ id.release });
    else
    {
        const int p = dragHandle == 0 ? id.attackCurve : dragHandle == 1 ? id.decayCurve : id.releaseCurve;
        beginParams ({ p });
        startValue = ctx.proc.params.get (p);
    }
    repaint();
}

void EnvGraph::mouseDrag (const MouseEvent& e)
{
    if (dragPoint < 0 && dragHandle < 0) return;
    const auto& id = ids().env[env];
    auto& proc = ctx.proc;
    const auto r = plot();
    const float t = jmax (0.0f, (e.position.x - r.getX()) / r.getWidth() * dragView);
    const auto s = settingsOf (env);
    auto setTime = [&] (int p, float seconds) { proc.set (p, jlimit (params()[p].min, params()[p].max, fromSeconds (jmax (0.0f, seconds)))); };
    switch (dragPoint)
    {
        case 1: setTime (id.attack, t); break;
        case 2: setTime (id.hold, t - s.attack); break;
        case 3:
        {
            setTime (id.decay, t - s.attack - s.hold);
            const float lvl = jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight());
            proc.set (id.sustain, lvl <= 0.001f ? -60.0f : jmax (-60.0f, 20.0f * std::log10 (lvl)));
            break;
        }
        case 4: setTime (id.release, t - s.attack - s.hold - s.decay); break;
        default:
        {
            const int p = dragHandle == 0 ? id.attackCurve : dragHandle == 1 ? id.decayCurve : id.releaseCurve;
            proc.set (p, jlimit (-1.0f, 1.0f, startValue - (float) e.getDistanceFromDragStartY() / 60.0f));
            break;
        }
    }
    refresh();
}

void EnvGraph::mouseUp (const MouseEvent&)
{
    for (int p : gestureParams) ctx.proc.endGesture (p);
    gestureParams.clear();
    dragPoint = dragHandle = -1;
    repaint();
}

void EnvGraph::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    if (locked) { zoomSeconds = viewLength(); locked = false; setUi (ctx.proc, "envLocked", false); }
    zoomSeconds = jlimit (0.01f, 60.0f, zoomSeconds * (w.deltaY > 0 ? 0.8f : 1.25f));
    setUi (ctx.proc, "envZoom", zoomSeconds);
    repaint();
}

// ---------------------------------------------------------------------------
Rectangle<float> EnvZoomStrip::lockBox() const { return { 0.0f, 0.0f, (float) getWidth(), 20.0f }; }
Rectangle<float> EnvZoomStrip::upBox() const   { return { 0.0f, 23.0f, (float) getWidth(), 16.0f }; }
Rectangle<float> EnvZoomStrip::zoomBox() const { return { 0.0f, 42.0f, (float) getWidth(), (float) getHeight() - 61.0f }; }
Rectangle<float> EnvZoomStrip::downBox() const { return { 0.0f, (float) getHeight() - 16.0f, (float) getWidth(), 16.0f }; }

void EnvZoomStrip::paint (Graphics& g)
{
    auto box = [&] (Rectangle<float> r, bool lit)
    {
        g.setColour (lit ? colours::cream : colours::black);
        g.fillRect (r.reduced (0.5f));
        g.setColour (colours::ink.withAlpha (0.8f));
        g.drawRect (r.reduced (0.5f), 1.0f);
        return lit ? colours::black : colours::ink;
    };
    // Lock.
    {
        const auto r = lockBox();
        const auto c = box (r, graph.locked);
        const auto body = Rectangle<float> (10.0f, 7.0f).withCentre (r.getCentre().translated (0.0f, 2.5f));
        g.setColour (c);
        g.fillRect (body);
        Path shackle;
        const float sx = graph.locked ? body.getX() + 2.0f : body.getX() + 4.0f;
        shackle.startNewSubPath (sx, body.getY());
        shackle.lineTo (sx, body.getY() - 3.0f);
        shackle.quadraticTo (sx + 3.0f, body.getY() - 7.0f, sx + 6.0f, body.getY() - 3.0f);
        if (graph.locked) shackle.lineTo (sx + 6.0f, body.getY());
        g.strokePath (shackle, PathStrokeType (1.3f));
    }
    auto arrow = [&] (Rectangle<float> r, bool up)
    {
        const auto c = box (r, false);
        const auto m = r.getCentre();
        Path p;
        if (up) p.addTriangle (m.x - 4.0f, m.y + 2.0f, m.x + 4.0f, m.y + 2.0f, m.x, m.y - 3.0f);
        else p.addTriangle (m.x - 4.0f, m.y - 2.0f, m.x + 4.0f, m.y - 2.0f, m.x, m.y + 3.0f);
        g.setColour (c);
        g.fillPath (p);
    };
    arrow (upBox(), true);
    arrow (downBox(), false);
    // Zoom (magnifier): drag up / down.
    {
        const auto r = zoomBox();
        const auto c = box (r, zoomDrag);
        const auto m = r.getCentre().translated (-1.0f, -1.0f);
        g.setColour (c);
        g.drawEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (m), 1.3f);
        g.drawLine (m.x + 3.0f, m.y + 3.0f, m.x + 6.0f, m.y + 6.0f, 1.6f);
    }
}

void EnvZoomStrip::mouseDown (const MouseEvent& e)
{
    auto& proc = contextOf (*this).proc;
    zoomDrag = false;
    auto unlock = [&]
    {
        if (graph.locked) { graph.zoomSeconds = graph.viewLength(); graph.locked = false; proc.uiState.setProperty ("envLocked", false, nullptr); }
    };
    if (lockBox().contains (e.position))
    {
        if (! graph.locked) graph.locked = true;
        else unlock();
        proc.uiState.setProperty ("envLocked", graph.locked, nullptr);
    }
    else if (upBox().contains (e.position)) { unlock(); graph.zoomSeconds = jmax (0.01f, graph.zoomSeconds / 1.5f); }
    else if (downBox().contains (e.position)) { unlock(); graph.zoomSeconds = jmin (60.0f, graph.zoomSeconds * 1.5f); }
    else { unlock(); zoomDrag = true; startZoom = graph.zoomSeconds; }
    proc.uiState.setProperty ("envZoom", graph.zoomSeconds, nullptr);
    graph.repaint();
    repaint();
}

void EnvZoomStrip::mouseDrag (const MouseEvent& e)
{
    if (! zoomDrag) return;
    // Up = zoom in (shorter view).
    graph.zoomSeconds = jlimit (0.01f, 60.0f, startZoom * std::pow (2.0f, (float) e.getDistanceFromDragStartY() / 40.0f));
    contextOf (*this).proc.uiState.setProperty ("envZoom", graph.zoomSeconds, nullptr);
    graph.repaint();
}

// ===========================================================================
// ENV panel
// ===========================================================================
EnvPanel::EnvPanel (PanelContext& c) : graph (c), ctx (c)
{
    ctx.add (this);
    for (int e = 0; e < kNumEnvs; ++e)
    {
        tabs[e] = std::make_unique<SourceTile> (ctx, envSource (e), "ENV " + String (e + 1));
        tabs[e]->onClick = [this, e] { select (e); };
        addAndMakeVisible (*tabs[e]);
    }
    addAndMakeVisible (graph);
    addAndMakeVisible (zoom);
    const auto& id = ids().env[0];
    const int ps[5] { id.attack, id.hold, id.decay, id.sustain, id.release };
    for (int i = 0; i < 5; ++i)
    {
        values[i] = std::make_unique<ParamField> (ctx, ps[i]);
        values[i]->fontH = 11.0f;
        addAndMakeVisible (*values[i]);
        knobs[i] = std::make_unique<ParamKnob> (ctx, ps[i]);
        knobs[i]->small = true;
        knobs[i]->accent = kEnvColour;
        addAndMakeVisible (*knobs[i]);
    }
    legInv = std::make_unique<ParamToggle> (ctx, id.legatoInvert, "L");
    legInv->setFontHeight (11.0f);
    addAndMakeVisible (*legInv);
    for (auto* b : { &bpmBtn, &msBtn })
    {
        b->setFontHeight (10.5f);
        b->setLitColour (Colour (0xff7fb8ff));
        addAndMakeVisible (*b);
    }
    bpmBtn.onClick = [this] (const ModifierKeys&) { setBpm (true); };
    msBtn.onClick = [this] (const ModifierKeys&) { setBpm (false); };
    bpmBtn.setTooltip ("Envelope times in note values (at the host tempo)");
    msBtn.setTooltip ("Envelope times in milliseconds / seconds");
    select (jlimit (0, kNumEnvs - 1, (int) getUi (ctx.proc, "modEnv", 0)));
}

EnvPanel::~EnvPanel() { ctx.remove (this); }

void EnvPanel::select (int e)
{
    sel = e;
    setUi (ctx.proc, "modEnv", e);
    for (int i = 0; i < kNumEnvs; ++i) { tabs[i]->selected = i == e; tabs[i]->repaint(); }
    graph.env = e;
    const auto& id = ids().env[e];
    const int ps[5] { id.attack, id.hold, id.decay, id.sustain, id.release };
    for (int i = 0; i < 5; ++i) { values[i]->setParam (ps[i]); knobs[i]->setParam (ps[i]); }
    legInv->setParam (id.legatoInvert);
    shownBpm = ! ctx.proc.params.on (id.bpm);   // force applyFormats
    refresh();
    graph.repaint();
}

void EnvPanel::applyFormats()
{
    const bool bpm = ctx.proc.params.on (ids().env[sel].bpm);
    shownBpm = bpm;
    bpmBtn.setLit (bpm);
    msBtn.setLit (! bpm);
    for (int i = 0; i < 5; ++i)
    {
        if (i == 3) continue;   // sustain is a level
        auto* k = knobs[i].get();
        if (bpm)
        {
            values[i]->format = [] (float plain) { return beatsText (plain); };
            k->format = [k] (double n) { return beatsText (params()[k->param()].fromNorm ((float) n)); };
        }
        else
        {
            values[i]->format = nullptr;
            k->format = [k] (double n) { return knobFormat (k->param(), n); };
        }
        values[i]->repaint();
    }
}

void EnvPanel::setBpm (bool on)
{
    const int e = sel;
    if (ctx.proc.params.on (ids().env[e].bpm) == on) return;
    const double tempo = hostBpm (ctx.proc);
    ctx.proc.editAll (on ? "Envelope BPM" : "Envelope MS", [e, on, tempo] (Patch&, ParamValues& v)
    {
        const auto& id = ids().env[e];
        v[id.bpm] = on ? 1.0f : 0.0f;
        for (int p : { id.attack, id.hold, id.decay, id.release })
        {
            const float conv = on ? envSnapSecondsToBeats (v[p], tempo) : (float) envBeatsToSeconds (v[p], tempo);
            v[p] = jlimit (params()[p].min, params()[p].max, conv);
        }
    });
    applyFormats();
    graph.repaint();
}

void EnvPanel::refresh()
{
    if (ctx.proc.params.on (ids().env[sel].bpm) != shownBpm) applyFormats();
}

void EnvPanel::resized()
{
    for (int e = 0; e < kNumEnvs; ++e) tabs[e]->setBounds (8 + e * 66, 14, 62, 20);
    graph.setBounds (8, 38, 314, 92);
    zoom.setBounds (326, 38, 18, 92);
    for (int i = 0; i < 5; ++i)
    {
        values[i]->setBounds (8 + i * 56, 134, 52, 17);
        knobs[i]->setBounds (8 + i * 56, 154, 52, 54);
    }
    legInv->setBounds (326, 134, 18, 17);
    bpmBtn.setBounds (288, 162, 56, 18);
    msBtn.setBounds (288, 184, 56, 18);
}

void EnvPanel::paint (Graphics&) {}

// ===========================================================================
// VELO / NOTE
// ===========================================================================
VeloNotePanel::VeloNotePanel (PanelContext& c) : ctx (c)
{
    ctx.add (this);
    auto& proc = ctx.proc;
    const char* names[] { "VELO", "NOTE" };
    const ModSource srcs[] { ModSource::Velocity, ModSource::Note };
    for (int i = 0; i < 2; ++i)
    {
        tabs[i] = std::make_unique<SourceTile> (ctx, srcs[i], names[i]);
        tabs[i]->onClick = [this, i] { select (i); };
        // §7: Legato (Portamento Time), Init Graph, then the source items.
        tabs[i]->menuExtras = [&proc, i] (Menu& m)
        {
            const auto& gr = proc.patch().graphs;
            const bool leg = i == 0 ? gr.veloLegato : gr.noteLegato;
            m.toggle ("Legato (Portamento Time)", leg, [&proc, i]
            {
                proc.editPatch ("Legato (Portamento Time)", [i] (Patch& p) { auto& b = i == 0 ? p.graphs.veloLegato : p.graphs.noteLegato; b = ! b; });
            });
            m.add ("Init Graph", [&proc, i] { proc.editPatch ("Init Graph", [i] (Patch& p) { (i == 0 ? p.graphs.velo : p.graphs.note) = Curve(); }); });
            m.separator();
        };
        addAndMakeVisible (*tabs[i]);
    }
    graph.colour = PanelContext::sourceColour (ModSource::Velocity);
    graph.gridX = graph.gridY = 4;
    graph.getCurve = [this] { const auto& g = ctx.proc.patch().graphs; return sel == 0 ? g.velo : g.note; };
    graph.onEdit = [this] (const Curve& cv)
    {
        const int s = sel;
        ctx.proc.editPatch ({}, [s, cv] (Patch& p) { (s == 0 ? p.graphs.velo : p.graphs.note) = cv; });
    };
    graph.onCommit = [this] (const String& label) { ctx.proc.commitHistory (label); };
    addAndMakeVisible (graph);
    select (jlimit (0, 1, (int) getUi (ctx.proc, "modVeloNote", 0)));
}

VeloNotePanel::~VeloNotePanel() { ctx.remove (this); }

void VeloNotePanel::select (int w)
{
    sel = w;
    setUi (ctx.proc, "modVeloNote", w);
    for (int i = 0; i < 2; ++i) { tabs[i]->selected = i == w; tabs[i]->repaint(); }
    graph.selection.clear();
    graph.update();
    repaint();
}

void VeloNotePanel::refresh()
{
    const auto& t = ctx.proc.telemetry();
    // The last note's input (velocity / note number) as a playhead.
    float live = -1.0f;
    if (t.lastNote.load() >= 0)
        live = sel == 0 ? jlimit (0.0f, 1.0f, t.lastVelocity.load()) : jlimit (0.0f, 1.0f, (float) t.lastNote.load() / 127.0f);
    graph.playhead = live;
    graph.update();
}

void VeloNotePanel::resized()
{
    tabs[0]->setBounds (8, 14, 56, 20);
    tabs[1]->setBounds (68, 14, 56, 20);
    graph.setBounds (8, 38, 116, 156);
}

void VeloNotePanel::paint (Graphics& g)
{
    const auto r = Rectangle<float> (8.0f, 196.0f, 116.0f, 13.0f);
    drawText (g, "0", r, 10.5f, colours::inkDim, false, Justification::centredLeft);
    drawText (g, "127", r, 10.5f, colours::inkDim, false, Justification::centredRight);
    drawText (g, sel == 0 ? "VELOCITY" : "NOTE", r, 10.5f, colours::inkDim, true);
}

// ===========================================================================
// VOICING / PORTAMENTO
// ===========================================================================
Menu PolyField::polyMenu()
{
    auto& proc = ctx2.proc;
    const int p = param();
    const int steal = ids().global.voiceSteal;
    Menu m;
    m.header ("Poly Count");
    m.toggle ("Limit Same Note Poly to 1", proc.patch().limitSameNote, [&proc]
    {
        proc.editPatch ("Limit Same Note Poly", [] (Patch& pp) { pp.limitSameNote = ! pp.limitSameNote; });
    });
    Menu s;
    const auto& names = params()[steal].choices;
    const int cur = proc.params.geti (steal);
    for (int i = 0; i < (int) names.size(); ++i)
        s.add (names[(size_t) i], [&proc, steal, i] { proc.setWithUndo (steal, (float) i); }, i == cur);
    m.sub ("Voice Steal Priority", s);
    m.separator();
    m.add ("Reset Control", [&proc, p] { proc.setWithUndo (p, params()[p].def); });
    m.toggle ("Lock Parameter", proc.isLocked (p), [&proc, p] { proc.setLocked (p, ! proc.isLocked (p)); });
    return m;
}

void PolyField::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), polyMenu()); return; }
    ParamField::mouseDown (e);
}

PortaCurve::PortaCurve (PanelContext& c, int p) : Bound (c, p) { refresh(); }

void PortaCurve::refresh()
{
    const float n = params()[paramId].toNorm (ctx.proc.params.get (paramId));
    if (! same (n, shownNorm)) { shownNorm = n; setTooltip (tooltipFor() + " (drag up / down)"); repaint(); }
}

void PortaCurve::paint (Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawLcdBox (g, b);
    const auto r = b.reduced (5.0f);
    g.setColour (colours::grid);
    g.drawLine (r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);
    const float k = ctx.proc.params.get (paramId);
    Path p;
    for (int i = 0; i <= 24; ++i)
    {
        const float t = (float) i / 24.0f;
        const Point<float> q (r.getX() + t * r.getWidth(), r.getBottom() - Curve::bend (t, k) * r.getHeight());
        if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
    }
    g.setColour (colours::lcd);
    g.strokePath (p, PathStrokeType (1.6f));
}

void PortaCurve::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), controlMenu (ctx, paramId)); return; }
    if (e.mods.isCommandDown()) { ctx.proc.setWithUndo (paramId, params()[paramId].def); return; }
    startNorm = params()[paramId].toNorm (ctx.proc.params.get (paramId));
    dragging = false;
}

void PortaCurve::mouseDrag (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    if (! dragging) { dragging = true; ctx.proc.beginGesture (paramId); }
    const float n = jlimit (0.0f, 1.0f, startNorm - (float) e.getDistanceFromDragStartY() / 120.0f);
    ctx.proc.set (paramId, params()[paramId].fromNorm (n));
    refresh();
}

void PortaCurve::mouseUp (const MouseEvent&)
{
    if (dragging) ctx.proc.endGesture (paramId);
    dragging = false;
}

void PortaCurve::mouseDoubleClick (const MouseEvent&) { ctx.proc.setWithUndo (paramId, params()[paramId].def); }

VoicingPanel::VoicingPanel (PanelContext& c) : ctx (c)
{
    ctx.add (this);
    const auto& g = ids().global;
    mono = std::make_unique<ParamToggle> (ctx, g.mono, "MONO");
    legato = std::make_unique<ParamToggle> (ctx, g.legato, "LEGATO");
    always = std::make_unique<ParamToggle> (ctx, g.portaAlways, "ALWAYS");
    scaled = std::make_unique<ParamToggle> (ctx, g.portaScaled, "SCALED");
    for (auto* t : { mono.get(), legato.get(), always.get(), scaled.get() }) { t->setFontHeight (11.0f); addAndMakeVisible (*t); }
    poly = std::make_unique<PolyField> (ctx, g.poly);
    poly->fontH = 13.0f;
    addAndMakeVisible (*poly);
    auto tri = [] (bool up)
    {
        return [up] (Graphics& gr, Rectangle<float> area, Colour colour)
        {
            const auto m = area.getCentre();
            Path p;
            if (up) p.addTriangle (m.x - 3.5f, m.y + 2.0f, m.x + 3.5f, m.y + 2.0f, m.x, m.y - 2.5f);
            else p.addTriangle (m.x - 3.5f, m.y - 2.0f, m.x + 3.5f, m.y - 2.0f, m.x, m.y + 2.5f);
            gr.setColour (colour);
            gr.fillPath (p);
        };
    };
    polyUp.icon = tri (true);
    polyDown.icon = tri (false);
    auto& proc = ctx.proc;
    const int pp = g.poly;
    polyUp.onClick = [&proc, pp] (const ModifierKeys&) { proc.setWithUndo (pp, jmin (params()[pp].max, proc.get (pp) + 1.0f)); };
    polyDown.onClick = [&proc, pp] (const ModifierKeys&) { proc.setWithUndo (pp, jmax (params()[pp].min, proc.get (pp) - 1.0f)); };
    addAndMakeVisible (polyUp);
    addAndMakeVisible (polyDown);
    porta = std::make_unique<ParamKnob> (ctx, g.porta, "PORTA");
    porta->small = true;
    addAndMakeVisible (*porta);
    curve = std::make_unique<PortaCurve> (ctx, g.portaCurve);
    addAndMakeVisible (*curve);
    refresh();
}

VoicingPanel::~VoicingPanel() { ctx.remove (this); }

String VoicingPanel::counterText() const
{
    auto& P = ctx.proc.params;
    int perNote = 0;
    for (int o = 0; o < kNumOscs; ++o)
        if (P.on (ids().osc[o].enable)) perNote += jmax (1, P.geti (ids().osc[o].unison));
    perNote = jmax (1, perNote);
    const int allowed = (P.on (ids().global.mono) ? 1 : P.geti (ids().global.poly)) * perNote;
    return String (ctx.proc.telemetry().activeUnison.load()) + " / " + String (allowed);
}

void VoicingPanel::refresh()
{
    const auto t = counterText();
    if (t != shownCounter) { shownCounter = t; repaint (counterBox); }
    poly->setEnabled (! ctx.proc.params.on (ids().global.mono));
}

void VoicingPanel::resized()
{
    mono->setBounds (8, 14, 76, 19);
    legato->setBounds (90, 14, 84, 19);
    poly->setBounds (8, 40, 146, 21);
    polyUp.setBounds (156, 40, 18, 11);
    polyDown.setBounds (156, 51, 18, 10);
    counterBox = { 8, 68, 166, 20 };
    porta->setBounds (6, 126, 56, 58);
    curve->setBounds (70, 128, 42, 42);
    always->setBounds (120, 130, 54, 19);
    scaled->setBounds (120, 154, 54, 19);
}

void VoicingPanel::paint (Graphics& g)
{
    auto r = counterBox.toFloat();
    drawText (g, "VOICES", r.removeFromLeft (58.0f), 10.5f, colours::inkDim, true, Justification::centredLeft);
    drawLcdBox (g, r);
    drawText (g, shownCounter, r, 13.0f, colours::lcd, true);
    drawText (g, "CURVE", { 66.0f, 171.0f, 50.0f, 14.0f }, 11.0f, colours::ink, true);
}

// ===========================================================================
// The strip
// ===========================================================================
ModStrip::ModStrip (PanelContext& ctx, OscPageHost& host) : macros (ctx), envs (ctx), lfos (ctx, host), veloNote (ctx), voicing (ctx)
{
    for (auto* c : std::initializer_list<Component*> { &macros, &envs, &lfos, &veloNote, &voicing }) addAndMakeVisible (c);
    setSize (kPageW, kModStripH);
}

void ModStrip::resized()
{
    macros.setBounds (macroArea());
    envs.setBounds (envArea());
    lfos.setBounds (lfoArea());
    veloNote.setBounds (veloArea());
    voicing.setBounds (voicingArea());
}

void ModStrip::paint (Graphics& g)
{
    drawSection (g, macroArea().toFloat(), "MACROS", PanelContext::sourceColour (ModSource::Macro1));
    drawSection (g, envArea().toFloat(), "ENVELOPES", kEnvColour);
    drawSection (g, lfoArea().toFloat(), "LFO", PanelContext::sourceColour (ModSource::Lfo1));
    drawSection (g, veloArea().toFloat(), "VELO / NOTE", PanelContext::sourceColour (ModSource::Velocity));
    const auto v = voicingArea().toFloat();
    drawSection (g, v.withHeight (98.0f), "VOICING");
    drawSection (g, v.withTrimmedTop (112.0f), "PORTAMENTO");
}

std::unique_ptr<Component> createModStrip (PanelContext& ctx, OscPageHost& host)
{
    return std::make_unique<ModStrip> (ctx, host);
}
} // namespace tincture::ui
