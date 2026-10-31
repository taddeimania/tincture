#include "ParamStore.h"

namespace tincture
{
// A host parameter that takes its range, steps and text from ParamInfo.
class HostParam final : public juce::RangedAudioParameter
{
public:
    HostParam (ParamStore& s, int idx)
        : juce::RangedAudioParameter (juce::ParameterID { juce::String (params()[idx].id), 1 }, juce::String (params()[idx].name),
                                      juce::AudioProcessorParameterWithIDAttributes().withLabel ({})),
          store (s), index (idx), pinfo (params()[idx])
    {
        range = juce::NormalisableRange<float> (pinfo.min, pinfo.max,
            [this] (float, float, float n) { return pinfo.fromNorm (n); },
            [this] (float, float, float v) { return pinfo.toNorm (v); },
            [this] (float, float, float v) { return pinfo.snap (v); });
        norm.store (pinfo.toNorm (pinfo.def));
    }

    float getValue() const override { return norm.load(); }
    void setValue (float n) override
    {
        norm.store (n);
        store.hostChanged (index, pinfo.fromNorm (n));
    }
    float getDefaultValue() const override { return pinfo.toNorm (pinfo.def); }
    int getNumSteps() const override
    {
        if (pinfo.isDiscrete()) return juce::jmax (2, (int) (pinfo.max - pinfo.min) + 1);
        return juce::AudioProcessor::getDefaultNumParameterSteps();
    }
    bool isDiscrete() const override { return pinfo.isDiscrete(); }
    bool isBoolean() const override { return pinfo.scale == ParamScale::Bool; }
    juce::String getText (float n, int) const override { return juce::String (pinfo.format (pinfo.fromNorm (n))); }
    float getValueForText (const juce::String& t) const override
    {
        float v = pinfo.def;
        pinfo.parse (t.toStdString(), v);
        return pinfo.toNorm (v);
    }
    juce::StringArray getAllValueStrings() const override
    {
        juce::StringArray a;
        if (pinfo.scale == ParamScale::Choice) for (auto& c : pinfo.choices) a.add (c);
        return a;
    }
    const juce::NormalisableRange<float>& getNormalisableRange() const override { return range; }

private:
    ParamStore& store;
    int index;
    const ParamInfo& pinfo;
    juce::NormalisableRange<float> range;
    std::atomic<float> norm { 0.0f };
};

ParamStore::ParamStore (juce::AudioProcessor& proc)
    : values ((size_t) params().size())
{
    const auto& reg = params();
    hostParams.assign ((size_t) reg.size(), nullptr);
    for (int i = 0; i < reg.size(); ++i)
    {
        values[(size_t) i].store (reg[i].def);
        if ((reg[i].flags & kNoAutomate) == 0)
        {
            auto p = std::make_unique<HostParam> (*this, i);
            hostParams[(size_t) i] = p.get();
            proc.addParameter (p.release());
        }
    }
}

void ParamStore::set (int i, float plain)
{
    const auto& pi = info (i);
    plain = pi.snap (plain);
    if (same (values[(size_t) i].load(), plain)) return;
    values[(size_t) i].store (plain);
    ++changeCounter;
    if (auto* h = hostParams[(size_t) i])
        h->setValueNotifyingHost (pi.toNorm (plain));
}

void ParamStore::beginGesture (int i) { if (auto* h = hostParams[(size_t) i]) h->beginChangeGesture(); }
void ParamStore::endGesture (int i)   { if (auto* h = hostParams[(size_t) i]) h->endChangeGesture(); }

void ParamStore::copyTo (ParamValues& out) const noexcept
{
    if (out.v.size() != values.size()) return;   // prepared elsewhere
    for (size_t i = 0; i < values.size(); ++i)
        out.v[i] = values[i].load (std::memory_order_relaxed);
}

void ParamStore::save (juce::ValueTree& into) const
{
    const auto& reg = params();
    for (int i = 0; i < reg.size(); ++i)
        into.setProperty (juce::Identifier (juce::String (reg[i].id)), get (i), nullptr);
}

void ParamStore::load (const juce::ValueTree& from, const std::vector<bool>* locked)
{
    const auto& reg = params();
    for (int i = 0; i < reg.size(); ++i)
    {
        if (locked != nullptr && (*locked)[(size_t) i]) continue;
        const juce::Identifier id (juce::String (reg[i].id));
        set (i, from.hasProperty (id) ? (float) from.getProperty (id) : reg[i].def);
    }
}

void ParamStore::resetAll (const std::vector<bool>* locked)
{
    const auto& reg = params();
    for (int i = 0; i < reg.size(); ++i)
        if (locked == nullptr || ! (*locked)[(size_t) i])
            set (i, reg[i].def);
}
} // namespace tincture
