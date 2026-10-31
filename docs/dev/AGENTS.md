# Rules for engine module work (Tincture)

Tincture is a JUCE/C++20 software synthesizer plugin whose feature set follows the Serum 2 user
guide. Feature specs (written from the guide) are in .ref/spec/A_*.md .. E_*.md (local only, not in git). Read the sections
for your module carefully; menus and parameter lists there are the reference.

## Read first
- Source/Engine/Core.h      constants (kTick=32 control-rate tick, voice/unison limits), Context, math, Rng
- Source/Engine/Params.h    parameter registry: plain values, normalised modulation space
- Source/Engine/ParamIds.h  every parameter's index (ids().osc[i].level etc.) and the choice lists
- Source/Engine/Module.h    ParamView (per-voice modulated param access), NoteInfo, OscTick, FilterTick
- Source/Engine/Warp.h, SampleData.h, Source/Model/Tree.h (state save/load), Source/Model/Curve.h

These shared headers are owned by the lead. Do NOT edit them (other agents are compiling against
them right now). If you truly need a change or a new parameter, work around it and list the request
in your final report. Do not edit files owned by other agents. You may only create/edit the files
listed as yours.

## Engine rules
- The engine (Source/Engine, Source/Model) has NO JUCE dependency. Plain C++20, must compile with
  GCC, Clang and MSVC (no GNU-only extensions without a fallback, no M_PI, no VLAs).
- Performance matters a lot (the user asked for "super performant"):
  - No allocation, locks, file I/O, exceptions or logging on the audio path (render/process calls).
    Allocate in prepare() / constructors / message-thread "build" functions.
  - Work in tick-sized blocks; compute coefficients once per tick; tight inner loops over plain
    float arrays that the compiler can vectorise (-O3 -ffast-math is used for the engine).
  - No virtual calls or std::function per sample. Avoid branches per sample where a per-tick
    switch can select a specialised loop.
  - Measure: include a micro-benchmark in your test program and report the numbers.
- Denormals are flushed by the caller (ScopedNoDenormals), but don't rely on it for feedback loops
  that can decay forever: add tiny DC/noise or snap to zero.
- Immutable shared data (wavetables, samples, IRs) is built off the audio thread and shared via
  std::shared_ptr<const T>.
- Patch data (anything that isn't a parameter) must save/load through tincture::Node (Tree.h):
  missing keys = defaults, unknown keys ignored.
- Style: match the existing files (4-space indent, `f (x)` call spacing, braces on own lines,
  a header comment saying what the file is). Plain, concrete comments.

## Building and testing (important: shared tree)
Several agents are writing code in this tree at the same time, so do NOT run cmake/ninja on the
project (someone else's half-written file would break your build and you'd waste time). Instead
compile only your files plus the core:

    CORE="Source/Engine/Core.cpp Source/Engine/Params.cpp Source/Engine/ParamIds.cpp \
          Source/Engine/FilterTypes.cpp Source/Engine/SampleData.cpp Source/Model/Tree.cpp Source/Model/Curve.cpp"
    g++ -std=c++20 -O3 -ffast-math -fno-finite-math-only -Wall -Wextra -ISource \
        $CORE <your .cpp files> Tests/Engine<YourModule>Tests.cpp -o /tmp/<you>/tests && /tmp/<you>/tests

Also check `clang++` compiles your files cleanly (-fsyntax-only is fine). Put your test program in
Tests/Engine<YourModule>Tests.cpp with its own main(); it must return non-zero on failure and
print a short summary. Test real behaviour (pitch accuracy via zero crossings / FFT, aliasing
levels, filter responses, timing accuracy, state round trips), not just "it runs".
Use /tmp/<your-agent-name>/ for scratch files.

## Report back
When done, reply with: files written; the public API (class names, key methods, the patch-data
structs and their save/load); test + benchmark results; spec items you didn't implement or
interpreted; and any change you need in shared headers. Keep it under ~60 lines.
