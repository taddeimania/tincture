// ParamStore.h — the plugin side of the parameter registry.
//
// Automatable parameters become host parameters (one juce parameter each,
// in registry order, id = registry id). The rest (patch settings such as
// types and modes, flagged kNoAutomate) are kept here as atomics and saved
// with the patch. Either way the UI and the processor use one interface,
// by registry index, in PLAIN units.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Engine/ParamIds.h"
#include "Engine/Params.h"

#include <atomic>
#include <memory>
#include <vector>

namespace tincture
{
class HostParam;

class ParamStore
{
public:
    explicit ParamStore (juce::AudioProcessor&);

    int size() const noexcept { return (int) values.size(); }
    const ParamInfo& info (int i) const { return params()[i]; }

    float get (int i) const noexcept { return values[(size_t) i].load (std::memory_order_relaxed); }
    float getNorm (int i) const { return info (i).toNorm (get (i)); }
    int geti (int i) const noexcept { return (int) std::lround (get (i)); }
    bool on (int i) const noexcept { return get (i) >= 0.5f; }

    // Message thread. Host parameters are told (and the host notified).
    void set (int i, float plain);
    void setNorm (int i, float norm) { set (i, info (i).fromNorm (norm)); }
    void reset (int i) { set (i, info (i).def); }
    void beginGesture (int i);
    void endGesture (int i);

    // Audio thread: copy every value into the engine's array.
    void copyTo (ParamValues& out) const noexcept;

    juce::RangedAudioParameter* host (int i) const noexcept { return hostParams[(size_t) i]; }
    int indexOf (const juce::String& id) const { return params().find (id.toStdString()); }

    // Called by HostParam when the host changes a value.
    void hostChanged (int i, float plain) noexcept { values[(size_t) i].store (plain, std::memory_order_relaxed); ++changeCounter; }

    // Bumped on every change: the UI uses it to know when to repaint.
    std::atomic<uint32_t> changeCounter { 0 };

    // Save / load every value by id.
    void save (juce::ValueTree& into) const;
    void load (const juce::ValueTree& from, const std::vector<bool>* lockedMask = nullptr);
    void resetAll (const std::vector<bool>* lockedMask = nullptr);

private:
    std::vector<std::atomic<float>> values;
    std::vector<juce::RangedAudioParameter*> hostParams;   // null for patch settings
};
} // namespace tincture
