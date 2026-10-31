// PluginProcessor.h — Tincture's processor: host parameters, the patch, the
// engine, undo history, presets, MIDI learn and locks. Everything the panel
// does goes through the public message-thread API here.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Engine/Synth.h"
#include "ParamStore.h"
#include "PresetStore.h"
#include "Settings.h"

#include <functional>
#include <memory>
#include <set>

class TinctureProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    TinctureProcessor();
    ~TinctureProcessor() override;

    // --- AudioProcessor -----------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return presetName(); }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- Parameters (message thread) ----------------------------------------
    tincture::ParamStore params;
    float get (int p) const { return params.get (p); }
    void set (int p, float plain) { params.set (p, plain); }
    // Wrap a UI drag/edit of a parameter: undo groups by gesture.
    void beginGesture (int p);
    void endGesture (int p);
    void setWithUndo (int p, float plain);         // one-shot change (menus, buttons)

    // --- The patch (message thread) -----------------------------------------
    const tincture::Patch& patch() const { return *patchNow; }
    // Change the patch. `fn` edits a copy, which then goes to the engine.
    // An empty label skips the undo history (continuous drawing: call
    // commitHistory() at the end of the gesture).
    void editPatch (const juce::String& undoLabel, const std::function<void (tincture::Patch&)>& fn);
    void commitHistory (const juce::String& label);

    // Edits that touch both parameters and the patch (matrix "Apply and Delete
    // Macros", Init Module, etc.) get a ParamValues snapshot to change.
    void editAll (const juce::String& undoLabel, const std::function<void (tincture::Patch&, tincture::ParamValues&)>& fn);

    // --- Undo -----------------------------------------------------------------
    bool canUndo() const { return historyPos > 0; }
    bool canRedo() const { return historyPos + 1 < (int) history.size(); }
    juce::String undoLabel() const;
    juce::String redoLabel() const;
    void undo();
    void redo();

    // --- Presets ---------------------------------------------------------------
    tincture::PresetStore presets;
    juce::String presetName() const { return juce::String (patchNow->info.name); }
    bool presetEdited() const { return edited; }
    void initPatch();                               // Init Preset (clears locks)
    void initModule (const juce::String& group);    // a module's params to defaults, its modulations removed
    bool loadPreset (const juce::File&);
    bool loadPresetData (const juce::MemoryBlock&, const juce::String& nameIfMissing = {});
    bool savePreset (const juce::File&);
    void stepPreset (int delta);                    // < > in the header
    juce::File currentPresetFile() const { return presetFile; }
    void revertToSaved();
    void saveAsDefault();

    // Serum 2 presets (.SerumPreset): converted on load (Model/SerumImport.h).
    // `summary` gets the import report. loadPreset() also accepts these.
    bool importSerumPreset (const juce::File&, juce::String& summary);
    // Converts every .SerumPreset under `folder` into User/Serum Imports/<folder name>/...,
    // keeping the sub-folders; returns how many were written.
    int importSerumFolder (const juce::File& folder, juce::String& summary);
    static bool isSerumPreset (const juce::File&);

    // Module clipboard (Copy / Copy (w/mods) / Paste on module labels).
    void copyModule (const juce::String& group, bool withMods);
    bool canPasteModule (const juce::String& group) const;
    void pasteModule (const juce::String& group);

    // --- Locks and MIDI learn ------------------------------------------------
    bool isLocked (int p) const { return locked.count (p) > 0; }
    void setLocked (int p, bool);
    bool isModuleLocked (const juce::String& group) const;
    void setModuleLocked (const juce::String& group, bool);
    void startMidiLearn (int p) { learning.store (p); }
    int midiLearnTarget() const { return learning.load(); }
    void clearMidiLearn (int p);
    int ccFor (int p) const;                        // -1 none

    // --- Live data for the panel ---------------------------------------------
    const tincture::Telemetry& telemetry() const { return synth.telemetry; }
    tincture::Telemetry& telemetry() { return synth.telemetry; }
    const tincture::MatrixTelemetry& matrixTelemetry() const { return synth.matrixTelemetry; }
    const tincture::SeqTelemetry& seqTelemetry() const { return synth.seqTelemetry(); }
    tincture::FxEngine& fxEngine() { return synth.fxEngine(); }
    void post (const tincture::SeqCommand& c) { synth.post (c); }
    // Playheads and grains of the newest voice's sample-mode oscillator, or null when nothing plays.
    const tincture::SampleOsc::Telemetry* sampleTelemetry (int osc) const
    {
        const int v = synth.telemetry.newestVoice.load();
        return v < 0 ? nullptr : &synth.sampleTelemetry (v, osc);
    }

    // On-screen keyboard and wheels.
    void uiNoteOn (int note, float velocity);
    void uiNoteOff (int note);
    void uiController (int cc, int value);
    void uiPitchBend (float minus1to1);

    // Quality (GLOBAL page): 1 = Good, 2 = High, 4 = Ultra.
    int quality() const { return oversample; }
    void setQuality (int factor);

    // Panel state saved with the session (selected tab, LFO shown, ...).
    juce::ValueTree uiState { "ui" };
    tincture::Settings settings;

    // Sample / wavetable files (message thread; decodes and edits the patch).
    bool loadSampleFile (int osc, const juce::File&);      // OSC A..C in a sample mode
    bool loadNoiseFile (const juce::File&);
    bool loadWavetableFile (int osc, const juce::File&, int importMode = 0);
    static std::shared_ptr<const tincture::SampleData> decodeAudio (const juce::File&);

private:
    void timerCallback() override;
    void publish();                                  // compile the patch and hand it to the engine
    void recordHistory (const juce::String& label);
    juce::MemoryBlock snapshot() const;
    void restore (const juce::MemoryBlock&, bool keepLocks);

    tincture::Synth synth;
    tincture::ParamValues pv;
    std::shared_ptr<tincture::Patch> patchNow;
    std::vector<tincture::MidiEvent> midiScratch;
    tincture::MidiBuffer midiOutScratch;

    struct HistoryItem { juce::String label; juce::MemoryBlock state; };
    std::vector<HistoryItem> history;
    int historyPos = -1;
    int gestureDepth = 0;
    juce::String gestureLabel;

    bool edited = false;
    juce::File presetFile;
    std::set<int> locked;
    std::set<juce::String> lockedModules;
    std::atomic<int> learning { -1 };
    std::array<std::atomic<int>, 128> ccMap;          // cc -> param, -1 none
    juce::MemoryBlock moduleClipboard;
    juce::String moduleClipboardGroup;

    // UI -> audio events (keyboard, wheels).
    tincture::SpscFifo<tincture::MidiEvent, 256> uiMidi;
    int oversample = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TinctureProcessor)
};
