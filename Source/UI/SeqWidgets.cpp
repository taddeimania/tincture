#include "SeqWidgets.h"
#include "../Settings.h"

#include <map>

namespace tincture::ui::seq
{
using namespace juce;

bool altHeld() { return ModifierKeys::currentModifiers.isAltDown(); }

String noteName (int n)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    n = jlimit (0, 127, n);
    return String (names[n % 12]) + String (n / 12 - 2);
}

int parseNoteName (const String& s)
{
    auto t = s.trim().toUpperCase();
    if (t.isEmpty()) return -1;
    if (t.containsOnly ("0123456789")) return jlimit (0, 127, t.getIntValue());
    static const int base[] { 9, 11, 0, 2, 4, 5, 7 };   // A..G
    const juce_wchar c = t[0];
    if (c < 'A' || c > 'G') return -1;
    int n = base[c - 'A'];
    int i = 1;
    if (t[i] == '#') { ++n; ++i; }
    else if (t[i] == 'B' && t.length() > 2) { --n; ++i; }
    const auto oct = t.substring (i);
    if (oct.isEmpty() || ! oct.trimCharactersAtStart ("-").containsOnly ("0123456789")) return -1;
    return jlimit (0, 127, n + (oct.getIntValue() + 2) * 12);
}

// ---------------------------------------------------------------------------
namespace
{
int dragDepth = 0;
String pendingLabel;
}

void edit (PanelContext& ctx, const String& label, const std::function<void (Patch&)>& fn)
{
    if (dragDepth > 0) { pendingLabel = label; ctx.proc.editPatch ({}, fn); }
    else ctx.proc.editPatch (label, fn);
}

void beginDrag() { ++dragDepth; }

void endDrag (PanelContext& ctx)
{
    if (dragDepth > 0 && --dragDepth == 0 && pendingLabel.isNotEmpty())
    {
        ctx.proc.commitHistory (pendingLabel);
        pendingLabel.clear();
    }
}

String formatLength (double beats)
{
    const int sixteenths = jmax (0, roundToInt (beats * 4.0));
    return String (sixteenths / 16) + ". " + String ((sixteenths / 4) % 4) + ". " + String (sixteenths % 4);
}

double lengthStepFor (float f) { return f < 0.36f ? 4.0 : (f < 0.68f ? 1.0 : 0.25); }

bool parseLength (const String& t, double& beats)
{
    auto parts = StringArray::fromTokens (t.trim(), ". :", "");
    parts.removeEmptyStrings();
    if (parts.isEmpty() || parts.size() > 3) return false;
    for (auto& p : parts) if (! p.containsOnly ("0123456789")) return false;
    beats = parts[0].getIntValue() * 4.0 + (parts.size() > 1 ? parts[1].getIntValue() : 0) + (parts.size() > 2 ? parts[2].getIntValue() * 0.25 : 0.0);
    return beats > 0.0;
}

Menu listMenu (const std::vector<std::string>& items, int current, std::function<void (int)> onPick)
{
    Menu m;
    for (int i = 0; i < (int) items.size(); ++i)
        m.add (items[(size_t) i], [onPick, i] { onPick (i); }, i == current);
    return m;
}

Menu settingMenu (const String& title, const std::function<void (Menu&)>& extra, std::function<void()> onReset)
{
    Menu m;
    m.header (title);
    if (extra) { extra (m); m.separator(); }
    m.sub ("Mod Source", Menu(), false, false);
    m.add ("Bypass Modulator", [] {}, false, false);
    m.add ("Remove Modulator", [] {}, false, false);
    m.add ("Remove All Modulators", [] {}, false, false);
    m.separator();
    m.add ("Reset Control", std::move (onReset));
    return m;
}

void drawArrow (Graphics& g, Rectangle<float> r, int dir, Colour c)
{
    const auto ctr = r.getCentre();
    const float s = jmin (r.getWidth(), r.getHeight()) * 0.32f;
    Path p;
    switch (dir)
    {
        case 0:  p.addTriangle (ctr.x + s * 0.6f, ctr.y - s, ctr.x + s * 0.6f, ctr.y + s, ctr.x - s * 0.7f, ctr.y); break;
        case 1:  p.addTriangle (ctr.x - s * 0.6f, ctr.y - s, ctr.x - s * 0.6f, ctr.y + s, ctr.x + s * 0.7f, ctr.y); break;
        case 2:  p.addTriangle (ctr.x - s, ctr.y + s * 0.6f, ctr.x + s, ctr.y + s * 0.6f, ctr.x, ctr.y - s * 0.7f); break;
        default: p.addTriangle (ctr.x - s, ctr.y - s * 0.6f, ctr.x + s, ctr.y - s * 0.6f, ctr.x, ctr.y + s * 0.7f); break;
    }
    g.setColour (c);
    g.fillPath (p);
}

// ===========================================================================
// Field
// ===========================================================================
Field::Field (PanelContext& c, const String& cap, int capW) : ctx (c), caption (cap), captionW (capW)
{
    ctx.add (this);
    setRepaintsOnMouseActivity (false);
}

Field::~Field() { ctx.remove (this); }

Rectangle<float> Field::box() const
{
    auto r = getLocalBounds().toFloat();
    if (caption.isNotEmpty() && captionW > 0) r.removeFromLeft ((float) captionW);
    return r.reduced (0.5f);
}

void Field::refresh()
{
    const auto t = text ? text() : String();
    const bool a = active ? active() : true;
    if (t != shown || a != shownActive) { shown = t; shownActive = a; repaint(); }
}

void Field::paint (Graphics& g)
{
    if (! text) return;
    shown = text();
    shownActive = active ? active() : true;
    auto r = getLocalBounds().toFloat();
    if (caption.isNotEmpty() && captionW > 0)
        drawText (g, caption, r.removeFromLeft ((float) captionW), 11.0f, shownActive ? colours::ink : colours::inkDim, true, Justification::centredLeft);
    const auto b = box();
    drawLcdBox (g, b);
    auto t = b.reduced (3.0f, 0.0f);
    const Colour tc = shownActive ? textColour : textColour.withMultipliedAlpha (0.4f);
    if (arrows)
    {
        drawArrow (g, t.removeFromLeft (12.0f), 0, colours::inkDim);
        drawArrow (g, t.removeFromRight (12.0f), 1, colours::inkDim);
    }
    if (dropArrow)
        drawArrow (g, t.removeFromRight (11.0f), 3, colours::inkDim);
    drawText (g, shown, t, fontH, tc, true, centred ? Justification::centred : Justification::centredLeft);
}

void Field::mouseDown (const MouseEvent& e)
{
    popup = e.mods.isPopupMenu();
    dragging = moved = arrowHit = false;
    stepsDone = 0;
    if (popup)
    {
        if (contextMenu) showContextMenu (*this, e.getPosition(), contextMenu());
        else if (menu) showMenu (*this, menu());
        return;
    }
    const auto b = box();
    downFrac = jlimit (0.0f, 1.0f, (e.position.x - b.getX()) / jmax (1.0f, b.getWidth()));
    if (arrows && onStep)
    {
        if (e.position.x < b.getX() + 15.0f) { arrowHit = true; onStep (-1); return; }
        if (e.position.x > b.getRight() - 15.0f) { arrowHit = true; onStep (1); return; }
    }
}

void Field::mouseDrag (const MouseEvent& e)
{
    if (popup || arrowHit || ! onStep) return;
    if (! moved && e.getDistanceFromDragStart() < 3) return;
    moved = true;
    const int steps = roundToInt ((float) -e.getDistanceFromDragStartY() / (e.mods.isShiftDown() ? pixelsPerStep * 3.0f : pixelsPerStep));
    if (steps != stepsDone)
    {
        if (! dragging) { dragging = true; beginDrag(); }
        onStep (steps - stepsDone);
        stepsDone = steps;
        refresh();
    }
}

void Field::mouseUp (const MouseEvent& e)
{
    if (dragging) { dragging = false; endDrag (ctx); return; }
    if (popup || arrowHit || moved) return;
    if (onPlainClick) { onPlainClick (e.getPosition(), e.mods); return; }
    if (menu) showMenu (*this, menu());
}

void Field::mouseDoubleClick (const MouseEvent&)
{
    if (onDoubleClick) onDoubleClick();
}

void Field::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    if (onStep && ! same (w.deltaY, 0.0f)) { onStep (w.deltaY > 0 ? 1 : -1); refresh(); }
}

// ===========================================================================
// Check
// ===========================================================================
Check::Check (PanelContext& c, const String& l, bool b) : ctx (c), label (l), button (b)
{
    ctx.add (this);
}

Check::~Check() { ctx.remove (this); }

void Check::refresh()
{
    const int s = (ticked && ticked() ? 1 : 0) | (active && ! active() ? 2 : 0);
    if (s != shown) { shown = s; repaint(); }
}

void Check::paint (Graphics& g)
{
    const bool on = ticked && ticked();
    const bool act = ! active || active();
    shown = (on ? 1 : 0) | (act ? 0 : 2);
    auto r = getLocalBounds().toFloat();
    if (button)
    {
        r = r.reduced (1.0f);
        g.setColour (on ? accent : colours::black);
        g.fillRect (r);
        g.setColour (act ? colours::ink : colours::inkDim);
        g.drawRect (r, 1.5f);
        drawText (g, label, r, fontH, on ? colours::black : (act ? colours::ink : colours::inkDim), true);
        return;
    }
    const auto b = Rectangle<float> (12.0f, 12.0f).withCentre ({ r.getX() + 7.0f, r.getCentreY() });
    g.setColour (colours::black);
    g.fillRect (b);
    g.setColour (act ? colours::ink : colours::inkFaint);
    g.drawRect (b, 1.2f);
    if (on)
    {
        g.setColour (act ? accent : accent.withMultipliedAlpha (0.4f));
        g.fillRect (b.reduced (2.5f));
    }
    drawText (g, label, r.withTrimmedLeft (18.0f), fontH, act ? colours::ink : colours::inkDim, true, Justification::centredLeft);
}

void Check::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { if (contextMenu) showContextMenu (*this, e.getPosition(), contextMenu()); return; }
    if (onToggle) onToggle();
    refresh();
}

// ===========================================================================
// SeqKnob
// ===========================================================================
SeqKnob::SeqKnob (PanelContext& c, const String& caption) : ctx (c)
{
    ctx.add (this);
    knob.owner = this;
    knob.small = true;
    knob.caption = caption;
    knob.setRange (0.0, 1.0);
    knob.setPopupDisplayEnabled (false, false, nullptr);
    knob.format = [this] (double v) { return format ? format ((float) v) : String (v, 2); };
    knob.onValueChange = [this] { if (! syncing && set) set ((float) knob.getValue()); };
    knob.onDragStart = [] { beginDrag(); };
    knob.onDragEnd = [this] { endDrag (ctx); };
    addAndMakeVisible (knob);
}

SeqKnob::~SeqKnob() { ctx.remove (this); }

void SeqKnob::Inner::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { if (owner->contextMenu) showContextMenu (*this, e.getPosition(), owner->contextMenu()); return; }
    if (e.mods.isCommandDown()) { owner->set (owner->defaultNorm); owner->refresh(); return; }
    Knob::mouseDown (e);
}

void SeqKnob::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu() && contextMenu) showContextMenu (*this, e.getPosition(), contextMenu());
}

void SeqKnob::refresh()
{
    if (! get) return;
    const float n = get();
    const bool a = ! active || active();
    if (! same (n, shownNorm))
    {
        shownNorm = n;
        const ScopedValueSetter<bool> s (syncing, true);
        knob.setValue (n, dontSendNotification);
    }
    const auto t = format ? format (n) : String (n, 2);
    if (t != shownText || a != shownActive)
    {
        shownText = t;
        shownActive = a;
        knob.setAlpha (a ? 1.0f : 0.45f);
        repaint();
    }
}

void SeqKnob::resized()
{
    knob.setBounds (getLocalBounds().withTrimmedBottom (17));
}

void SeqKnob::paint (Graphics& g)
{
    const auto b = getLocalBounds().toFloat().removeFromBottom (16.0f).reduced (2.0f, 0.5f);
    drawLcdBox (g, b);
    drawText (g, shownText, b, 11.5f, shownActive ? colours::lcd : colours::lcdDim, true);
}

void SeqKnob::mouseDoubleClick (const MouseEvent&)
{
    if (! parse || ! ctx.editValue) return;
    SafePointer<SeqKnob> safe (this);
    ctx.editValue (*this, shownText, [safe] (const String& t)
    {
        float n;
        if (safe != nullptr && safe->parse (t, n)) { safe->set (jlimit (0.0f, 1.0f, n)); safe->refresh(); }
    });
}

// ===========================================================================
// MiniKeys
// ===========================================================================
namespace
{
constexpr int kWhiteOf[12] { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
constexpr bool kBlack[12] { false, true, false, true, false, false, true, false, true, false, true, false };
}

Rectangle<float> MiniKeys::keyBounds (int k) const
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float ww = r.getWidth() / 7.0f;
    if (! kBlack[k])
        return { r.getX() + ww * (float) kWhiteOf[k], r.getY(), ww, r.getHeight() };
    const float bw = ww * 0.62f;
    return { r.getX() + ww * (float) kWhiteOf[k - 1] + ww - bw * 0.5f, r.getY(), bw, r.getHeight() * 0.58f };
}

int MiniKeys::keyAt (Point<float> p) const
{
    for (int k = 0; k < 12; ++k) if (kBlack[k] && keyBounds (k).contains (p)) return k;
    for (int k = 0; k < 12; ++k) if (! kBlack[k] && keyBounds (k).contains (p)) return k;
    return -1;
}

void MiniKeys::update()
{
    for (int k = 0; k < 12; ++k)
    {
        const int s = (state ? state (k) : 0) | (hasContent && hasContent (k) ? 8 : 0);
        if (s != shown[k]) { repaint(); return; }
    }
}

void MiniKeys::paint (Graphics& g)
{
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 0; k < 12; ++k)
        {
            if (kBlack[k] != (pass == 1)) continue;
            const int st = state ? state (k) : 0;
            const bool content = hasContent && hasContent (k);
            shown[k] = st | (content ? 8 : 0);
            const auto b = keyBounds (k);
            Colour fill = kBlack[k] ? colours::black : colours::cream;
            if (st == 1) fill = accent;
            else if (st == 2) fill = accent.withMultipliedAlpha (0.45f);
            g.setColour (kBlack[k] ? colours::black : colours::cream);
            g.fillRect (b);
            g.setColour (fill);
            g.fillRect (b);
            g.setColour (colours::panelEdge);
            g.drawRect (b, 1.0f);
            if (st == 3)
            {
                g.setColour (accent);
                g.drawRect (b.reduced (1.0f), 2.0f);
            }
            if (content && st != 1)
            {
                // A small dot: this slot holds something.
                g.setColour (kBlack[k] ? colours::inkDim : colours::inkFaint);
                g.fillEllipse (Rectangle<float> (3.0f, 3.0f).withCentre ({ b.getCentreX(), b.getBottom() - 5.0f }));
            }
        }
}

void MiniKeys::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    const int k = keyAt (e.position);
    if (k >= 0 && onKey) onKey (k);
}

// ===========================================================================
// PowerButton
// ===========================================================================
PowerButton::PowerButton (PanelContext& c, int param, Colour a) : ParamToggle (c, param, {}), accent (a) {}

void PowerButton::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = isLit();
    g.setColour (on ? accent : colours::black);
    g.fillRect (r);
    g.setColour (colours::ink);
    g.drawRect (r, 1.2f);
    const auto c = r.getCentre();
    const float rad = jmin (r.getWidth(), r.getHeight()) * 0.27f;
    Path p;
    p.addCentredArc (c.x, c.y + 0.5f, rad, rad, 0.0f, 0.75f, MathConstants<float>::twoPi - 0.75f, true);
    g.setColour (on ? colours::black : colours::ink);
    g.strokePath (p, PathStrokeType (1.6f));
    g.drawLine (c.x, c.y - rad - 1.5f, c.x, c.y, 1.6f);
}

// ===========================================================================
// MidiOutBox
// ===========================================================================
MidiOutBox::MidiOutBox (PanelContext& c) : choice (c, ids().global.midiOut), ctx (c)
{
    ctx.add (this);
    choice.fontH = 12.0f;
    choice.getText = [this] { return String (params()[ids().global.midiOut].choices[(size_t) ctx.proc.params.geti (ids().global.midiOut)]).toUpperCase(); };
    choice.setTooltip ("MIDI OUT: what goes to the host (Off, the CLIP player only, or CLIP and ARP)");
    addAndMakeVisible (choice);
}

MidiOutBox::~MidiOutBox() { ctx.remove (this); }

void MidiOutBox::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromBottom (22);
    row.removeFromLeft (66);
    choice.setBounds (row);
}

void MidiOutBox::refresh()
{
    const auto n = ctx.proc.seqTelemetry().noteCount.load (std::memory_order_relaxed);
    if (n != shownCount) { shownCount = n; idleFrames = 0; repaint(); }
    else if (++idleFrames == 60) repaint();          // fade the dots
}

void MidiOutBox::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto row = r.removeFromBottom (22.0f);
    drawText (g, "MIDI OUT:", row.removeFromLeft (64.0f), 11.0f, colours::ink, true, Justification::centredLeft);
    r.removeFromBottom (5.0f);
    drawLcdBox (g, r);
    const auto d = r.reduced (4.0f);
    g.setColour (colours::grid);
    for (int i = 1; i < 4; ++i)
        g.fillRect (d.getX(), d.getY() + d.getHeight() * (float) i / 4.0f, d.getWidth(), 1.0f);
    const auto& t = ctx.proc.seqTelemetry();
    const uint32_t count = t.noteCount.load (std::memory_order_relaxed);
    const int shownN = (int) jmin<uint32_t> (count, (uint32_t) SeqTelemetry::kHistory);
    const bool stale = idleFrames >= 60;
    for (int i = 0; i < shownN; ++i)
    {
        const uint32_t v = t.notes[(count - 1 - (uint32_t) i) % SeqTelemetry::kHistory].load (std::memory_order_relaxed);
        const int pitch = (int) (v & 0x7f), vel = (int) ((v >> 8) & 0x7f), src = (int) ((v >> 16) & 0xff);
        const float x = d.getRight() - 4.0f - (float) i * (d.getWidth() - 8.0f) / (float) SeqTelemetry::kHistory;
        const float y = d.getBottom() - 3.0f - (d.getHeight() - 6.0f) * (float) jlimit (24, 108, pitch) / 132.0f * 1.2f + (d.getHeight() - 6.0f) * 0.2f;
        const Colour c = src == 1 ? clipColour : (src == 2 ? arpColour : colours::lcd);
        g.setColour (c.withAlpha ((stale ? 0.25f : 1.0f) * (1.0f - (float) i / (float) SeqTelemetry::kHistory * 0.7f)));
        const float s = 2.5f + 2.5f * (float) vel / 127.0f;
        g.fillEllipse (Rectangle<float> (s, s).withCentre ({ x, jlimit (d.getY() + 3.0f, d.getBottom() - 3.0f, y) }));
    }
    if (shownN == 0)
        drawText (g, "NO NOTES", d, 10.5f, colours::inkFaint, true);
}

// ===========================================================================
Menu tabMenu (PanelContext& ctx, bool clip)
{
    auto& proc = ctx.proc;
    Menu m;
    const bool locked = clip ? proc.patch().clips.lockModule : proc.patch().arp.lockModule;
    m.toggle ("Lock Module", locked, [&proc, clip]
    {
        proc.editPatch ("Lock Module", [clip] (Patch& p) { if (clip) p.clips.lockModule = ! p.clips.lockModule; else p.arp.lockModule = ! p.arp.lockModule; });
        proc.setModuleLocked (clip ? "Clip" : "Arp", clip ? proc.patch().clips.lockModule : proc.patch().arp.lockModule);
    });
    Menu oct;
    const int cur = clip ? proc.patch().clips.triggerOctave : proc.patch().arp.triggerOctave;
    const auto& list = seqchoices::triggerOctaves();
    for (int i = 0; i < (int) list.size(); ++i)
    {
        const int v = i - 1;
        oct.add (list[(size_t) i], [&proc, clip, v]
        {
            proc.editPatch ("MIDI Input Trigger Octave", [clip, v] (Patch& p) { if (clip) p.clips.triggerOctave = v; else p.arp.triggerOctave = v; });
        }, v == cur);
    }
    m.sub ("MIDI Input Trigger Octave", oct);
    return m;
}

void drawEditorBackground (Graphics& g, Rectangle<int> bounds, const String& title, Colour accent)
{
    g.setColour (colours::panel);
    g.fillRect (bounds);
    g.setColour (colours::panelEdge);
    g.drawRect (bounds, 1);
    const auto f = panelFont (14.0f, true);
    const auto tab = Rectangle<float> (10.0f, 6.0f, (float) GlyphArrangement::getStringWidth (f, title) + 20.0f, 20.0f);
    g.setColour (accent);
    g.fillRect (tab);
    drawText (g, title, tab, 14.0f, colours::panel, true);
}

// ===========================================================================
// Files
// ===========================================================================
File userFolder (const String& kind)
{
    auto f = tincture::Settings::userBanks().getChildFile (kind);
    f.createDirectory();
    return f;
}

Array<File> userFiles (const String& kind)
{
    auto files = userFolder (kind).findChildFiles (File::findFiles, false, "*.tseq");
    files.sort();
    return files;
}

File userFileFor (const String& kind, const String& name)
{
    return userFolder (kind).getChildFile (File::createLegalFileName (name) + ".tseq");
}

bool saveNode (const File& f, const Node& n)
{
    const auto bin = n.toBinary();
    return f.replaceWithData (bin.data(), bin.size());
}

bool loadNode (const File& f, Node& n)
{
    MemoryBlock mb;
    if (! f.loadFileAsData (mb)) return false;
    return Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), n);
}

namespace
{
struct Baseline { std::string name; std::vector<uint8_t> sig; };
std::map<std::pair<const void*, String>, Baseline>& baselines()
{
    static std::map<std::pair<const void*, String>, Baseline> b;
    return b;
}
}

void setBaseline (const void* owner, const String& kind, const std::string& name, std::vector<uint8_t> sig)
{
    baselines()[{ owner, kind }] = { name, std::move (sig) };
}

bool isModified (const void* owner, const String& kind, const std::string& name, const std::vector<uint8_t>& sig, const std::vector<uint8_t>& initSig)
{
    auto& b = baselines();
    auto it = b.find ({ owner, kind });
    if (it != b.end() && it->second.name == name) return it->second.sig != sig;
    if (name == "Init") return sig != initSig;
    return false;
}
} // namespace tincture::ui::seq
