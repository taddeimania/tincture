# Rules for UI work (Tincture)

Tincture is a JUCE 8 / C++20 synth plugin with the Serum 2 feature set (specs: .ref/spec/A_*.md .. E_*.md;
read the sections for your area — menu contents there are the reference, reproduce them in order and
wording). The engine is done and tested. You are building part of the panel.

## Look
Same flat hardware language as the user's other plugins (Planish, Burnish, ...): charcoal panel, cream
knob caps, outlined sections with an inverse tab title (drawSection), black LCD boxes with amber text,
flat LEDs, cream "lit" buttons. NOT Serum's look; no gradients, glows, glossy or "AI slop" styling.
The user dislikes cluttered/glossy UIs; keep it clean, aligned and legible. Everything you need is in
Source/UI/Widgets.h (colours::, panelFont, drawText, drawSection, drawLcdBox, drawLed, Knob, Fader,
PanelButton, ChoiceDisplay, Meter...). Real estate is tight (Serum packs a lot in): small knobs
(Knob::small) and LCD fields are fine; text must stay readable at 100% (>= 10.5 px).
Module accent colours: use colours::brand for oscillator things, Colour(0xff7fb8ff) for filters, the
mod source colours from PanelContext::sourceColour for modulators, FX colours from fxInfo(t).colour.

## Coordinates
Design space 1400 x 880, scaled to the window. Pages get kPageW x kPageH (1400 x 800) below the
62 px header, above an 18 px status line (UI/Pages.h has the constants and the OSC page bands).
Lay out with fixed numbers in that space.

## Binding and data flow (READ Source/UI/Bind.h, Menu.h, Pages.h, PluginProcessor.h first)
- Every control showing a parameter is a ParamKnob / ParamFader / ParamField / ParamToggle / ParamChoice
  (Bind.h). They handle drag, fine drag, reset, typed entry, right-click control menu, mod drops, halos.
  Parameter ids: tincture::ids() (Engine/ParamIds.h). Use `ctx.proc` (TinctureProcessor) for the rest.
- Non-parameter data lives in the Patch (Model/Patch.h). Read `ctx.proc.patch()`; change it ONLY with
  `ctx.proc.editPatch ("Undo label", [&] (Patch& p) { ... })` (or editAll for patch+params). For
  continuous drawing (LFO points, wavetable brush), edit with an empty label during the drag and call
  `ctx.proc.commitHistory ("Draw LFO")` on mouse-up. Never keep pointers into the patch across edits:
  every edit replaces it.
- Parameters set from code: `proc.setWithUndo (id, value)` for one-shot changes; drags via the bound
  widgets (they wrap gestures).
- Live data (envelope/LFO positions, playheads, voice counts, meters, matrix outputs, arp/clip state)
  comes from proc.telemetry(), proc.matrixTelemetry(), proc.seqTelemetry(), proc.fxEngine().
- Refresh: derive from tincture::ui::Refreshable and register with ctx.add(this) / ctx.remove(this)
  (Bound does this) — refresh() runs at 30 Hz on the message thread. Only repaint when something you
  draw changed. No per-component juce::Timer unless you animate continuously, and then only while visible.
- Commands to the sequencer: proc.post (SeqCommand {...}).

## Menus — top priority, the user has been burned by menu bugs before
- ALL menus go through tincture::ui::Menu + showMenu / showContextMenu (UI/Menu.h). Never use
  juce::PopupMenu, juce::ComboBox or any OS menu. Never call showMenu from a constructor or paint.
- Build the menu fresh when it opens (from current state): ticks show the current value; disabled items
  are disabled, not hidden, when the spec shows them greyed.
- Menu actions run after the menu has closed and may rebuild your component; capture by value / ids,
  not raw pointers into the patch. Use `ctx.proc` captured by reference (it outlives the panel).
- Long lists: use sub-menus by category as the spec shows; Menu handles columns/scrolling/search.
- Every parameter control's right-click menu is controlMenu() (Bind.h); add spec extras through the
  `extraMenu` hook (e.g. OCT/SEM "Mode >", PHASE "Phase Legato", WT POS "Smooth Interpolation",
  DRIVE "Clean Mode", LFO RATE "Swing"/"10x").
- Dropdown-style displays: ParamChoice (or ChoiceDisplay + your own Menu) with optional < > steppers;
  mouse wheel steps.

## Testing (required)
Use the shot harness (Tools/Shots.h): write Tools/Shots<YourArea>.cpp registering one or more
TINCTURE_SHOT("<name>", ...). In it:
- save PNGs of each of your views in their main states (and LOOK at them with the Read tool — fix
  overlaps, clipped text, misalignment, empty areas; iterate until it looks deliberate and tidy);
- open EVERY menu you built through a real click on the real component (s.click(comp, pt, right)),
  check its items against the spec (s.menuItems / s.menuHas), save a PNG of it open for the important
  ones, choose an item (s.choose("Sub/Item")) and check the effect on params/patch;
- exercise the main mouse gestures (drag points, draw, scroll, double-click) and check results.
Shots must pass (0 failures) before you report.

## Building (shared tree, several agents at once)
- Use YOUR OWN build directory so ninja runs don't collide:
      cd /home/claude/tincture
      cmake -B build-<you> -G Ninja -DTINCTURE_JUCE_DIR=/home/claude/JUCE -DTINCTURE_BUILD_TOOLS=ON -DTINCTURE_COPY_PLUGIN=OFF
      ninja -C build-<you> TinctureSnapshot
      xvfb-run -a build-<you>/TinctureSnapshot_artefacts/Release/TinctureSnapshot /tmp/<you> shot <name>
  (the first build compiles JUCE: several minutes; the machine has 2 cores shared by everyone).
- Source/UI/*.cpp and Tools/Shots*.cpp are globbed into the build, so a broken file breaks EVERYONE's
  build. Keep your files compiling at all times: write complete files, fix errors immediately. If a
  build fails in someone else's file, wait a minute and retry (don't edit their file).
- Only create/edit the files you own (listed in your brief). Shared files (Bind.*, Menu.*, Widgets.*,
  Pages.h, PluginEditor.*, PluginProcessor.*, Engine/*, Model/*) belong to the lead: if you need a
  change there, work around it locally and list the request in your report. Exception: you may ADD new
  small helper functions to your own files freely.
- Performance: paint only what changed; cache Paths/Images for static parts; no allocation-heavy work
  in paint for big views (piano roll, wavetable 3D) — cache and invalidate.

## Report
Reply with: files; what each view contains; menus implemented (names) and any spec menu you couldn't
do; shot results; the PNG names worth looking at; requests for shared files. Under ~50 lines.
