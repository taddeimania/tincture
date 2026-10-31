// NoteProcessor.cpp — the per-block MIDI pipeline (see NoteProcessor.h).
#include "NoteProcessor.h"
#include "Scales.h"

#include <algorithm>
#include <cmath>

namespace tincture
{
// ---------------------------------------------------------------------------
// Keyboard settings
// ---------------------------------------------------------------------------
int NoteRange::map (int note, int velocity) const noexcept
{
    if (velocity < velLo || velocity > velHi) return -1;
    if (note >= keyLo && note <= keyHi) return note;
    if (! fold || keyHi - keyLo < 11) return -1;
    while (note < keyLo) note += 12;
    while (note > keyHi) note -= 12;
    return note >= keyLo && note <= keyHi ? note : -1;
}

void KeyboardSettings::save (Node& n) const
{
    n.set ("swingDiv", swingDivision);
    for (int i = 0; i < kNumRows; ++i)
    {
        const auto& r = mapping[(size_t) i];
        n.add ("range").set ("row", i).set ("keyLo", r.keyLo).set ("keyHi", r.keyHi).set ("velLo", r.velLo).set ("velHi", r.velHi)
         .set ("fold", r.fold ? 1.0 : 0.0).set ("warp", r.warp ? 1.0 : 0.0);
    }
}

void KeyboardSettings::load (const Node& n)
{
    *this = KeyboardSettings();
    swingDivision = std::clamp (n.integer ("swingDiv", 1), 0, 3);
    for (auto* c : n.children ("range"))
    {
        const int i = c->integer ("row", -1);
        if (i < 0 || i >= kNumRows) continue;
        auto& r = mapping[(size_t) i];
        r.keyLo = std::clamp (c->integer ("keyLo", 0), 0, 127);
        r.keyHi = std::clamp (c->integer ("keyHi", 127), r.keyLo, 127);
        r.velLo = std::clamp (c->integer ("velLo", 1), 1, 127);
        r.velHi = std::clamp (c->integer ("velHi", 127), r.velLo, 127);
        r.fold = c->flag ("fold");
        r.warp = c->flag ("warp");
    }
}

SeqTelemetry::SeqTelemetry()
{
    for (auto& p : clipPlayhead) p.store (-1.0f);
    for (auto& n : notes) n.store (0);
}

// ---------------------------------------------------------------------------
// NoteProcessor
// ---------------------------------------------------------------------------
NoteProcessor::NoteProcessor() { reset(); }

void NoteProcessor::prepare (const Context& ctx)
{
    sampleRate = ctx.sampleRate > 0.0 ? ctx.sampleRate : 48000.0;
    reset();
}

void NoteProcessor::reset() noexcept
{
    arp.reset();
    clip.reset();
    for (auto& c : consumed) c = kNone;
    for (auto& s : arpRoute) for (auto& r : s) r = -1;
    for (auto& s : map) for (auto& m : s) m = Mapping();
    for (int i = 0; i < 128; ++i) voiceCount[i] = hostCount[i] = clipHostCount[i] = 0;
    expectedBeat = 0.0;
    wasPlaying = false;
    first = true;
    prevArpOn = prevClipOn = false;
    prevMidiOut = 0;
}

void NoteProcessor::setKeyboardSettings (const KeyboardSettings& k) noexcept
{
    swingDivision.store (k.swingDivision);
    const auto& r = k.mapping[KeyboardSettings::Arp];
    arpKeyLo.store (r.keyLo);
    arpKeyHi.store (r.keyHi);
    arpVelLo.store (r.velLo);
    arpVelHi.store (r.velHi);
    arpFold.store (r.fold);
}

int NoteProcessor::kbSlot (const ClipBank& b) const noexcept
{
    const int s = clip.lastLaunched();
    return s >= 0 ? s : b.selected;
}

void NoteProcessor::trackOn (uint8_t* counts, int pitch, int vel, int offset, int channel, uint8_t src, MidiBuffer& out) noexcept
{
    if (counts[pitch] > 0) out.add (MidiEvent::noteOff (offset, pitch, 64, channel, src));     // retrigger
    out.add (MidiEvent::noteOn (offset, pitch, vel, channel, src));
    if (counts[pitch] < 255) ++counts[pitch];
}

void NoteProcessor::trackOff (uint8_t* counts, int pitch, int vel, int offset, int channel, uint8_t src, MidiBuffer& out) noexcept
{
    if (counts[pitch] == 0) return;
    if (--counts[pitch] == 0) out.add (MidiEvent::noteOff (offset, pitch, vel, channel, src));
}

void NoteProcessor::releaseHost (uint8_t* counts, int offset, MidiBuffer& toHost) noexcept
{
    for (int p = 0; p < 128; ++p)
        if (counts[p] > 0) { toHost.add (MidiEvent::noteOff (offset, p, 64, chan)); counts[p] = 0; }
}

void NoteProcessor::mapEvent (const MidiEvent& e, int transpose, int key, int scale, bool hostOn,
                              MidiBuffer& toVoices, MidiBuffer& toHost) noexcept
{
    const int src = std::min<int> (e.source, 2);
    const bool toHostToo = hostOn && src != MidiEvent::kInput;
    if (e.isNoteOn())
    {
        Mapping& m = map[src][e.note()];
        const int t = e.note() + transpose;
        const int target = t < 0 || t > 127 ? -1 : scales::quantise (t, key, scale);
        if (m.count > 0 && m.target != target)
        {
            // Same key again but it now maps elsewhere: end the old mapping first.
            for (; m.count > 0; --m.count)
            {
                if (m.target < 0) continue;
                trackOff (voiceCount, m.target, 64, e.offset, e.channel(), e.source, toVoices);
                if (toHostToo) trackOff (hostCount, m.target, 64, e.offset, chan, e.source, toHost);
            }
        }
        m.target = (int16_t) target;
        if (m.count < 255) ++m.count;
        if (target < 0) return;
        trackOn (voiceCount, target, e.velocity(), e.offset, e.channel(), e.source, toVoices);
        if (toHostToo) trackOn (hostCount, target, e.velocity(), e.offset, chan, e.source, toHost);
    }
    else if (e.isNoteOff())
    {
        Mapping& m = map[src][e.note()];
        if (m.count == 0) return;
        --m.count;
        if (m.target >= 0)
        {
            trackOff (voiceCount, m.target, e.velocity(), e.offset, e.channel(), e.source, toVoices);
            if (toHostToo) trackOff (hostCount, m.target, e.velocity(), e.offset, chan, e.source, toHost);
        }
        if (m.count == 0) m.target = -1;
    }
    else if (e.isPolyAftertouch())
    {
        const Mapping& m = map[src][e.note()];
        if (m.count > 0 && m.target >= 0)
        {
            MidiEvent x = e;
            x.data1 = (uint8_t) m.target;
            toVoices.add (x);
        }
    }
    else toVoices.add (e);
}

void NoteProcessor::allNotesOff (int offset, MidiBuffer& toVoices, MidiBuffer& toHost) noexcept
{
    for (int p = 0; p < 128; ++p)
        if (voiceCount[p] > 0) { toVoices.add (MidiEvent::noteOff (offset, p, 64, chan)); voiceCount[p] = 0; }
    releaseHost (hostCount, offset, toHost);
    releaseHost (clipHostCount, offset, toHost);
    for (auto& s : map) for (auto& m : s) m = Mapping();
    for (auto& s : arpRoute) for (auto& r : s) r = -1;
    for (auto& c : consumed) c = kNone;
    arp.reset();
    clip.reset();
}

void NoteProcessor::process (const MidiEvent* in, int numIn, int numSamples, const Context& ctx, const ParamValues& pv,
                             const ArpBank& arpBank, const ClipBank& clipBank, MidiBuffer& toVoices, MidiBuffer& toHost) noexcept
{
    toVoices.clear();
    toHost.clear();
    playable.clear(); clipOut.clear(); arpIn.clear(); arpOut.clear(); direct.clear(); finalStream.clear();
    if (numSamples <= 0) return;

    // ---- clock --------------------------------------------------------------
    const double bpm = std::max (1.0, ctx.bpm);
    const double sr = ctx.sampleRate > 0.0 ? ctx.sampleRate : sampleRate;
    BlockClock clk;
    clk.beatsPerSample = bpm / 60.0 / sr;
    clk.numSamples = numSamples;
    bool jump = false;
    if (ctx.playing)
    {
        clk.beat0 = ctx.ppq;
        if (! first && (! wasPlaying || std::abs (ctx.ppq - expectedBeat) > std::max (0.01, 4.0 * clk.beatsPerSample))) jump = true;
    }
    else clk.beat0 = first ? 0.0 : expectedBeat;     // host stopped: free-running internal clock
    expectedBeat = clk.endBeat();
    wasPlaying = ctx.playing;

    // ---- parameters ---------------------------------------------------------
    const auto& g = ids().global;
    const bool arpOn = pv[g.arpOn] >= 0.5f;
    const bool clipOn = pv[g.clipOn] >= 0.5f;
    const int transpose = (int) std::lround (pv[g.transpose]);
    const int key = scales::keyFromParam (pv[g.key]);
    const int scale = std::clamp ((int) std::lround (pv[g.scale]), 0, scales::kNumScales - 1);
    const int midiOut = std::clamp ((int) std::lround (pv[g.midiOut]), 0, 2);
    Swing swing;
    swing.division = swingDivisionBeats (swingDivision.load (std::memory_order_relaxed));
    swing.amount = clampf (pv[g.swing], 0.0f, 1.0f);

    if (midiOut != prevMidiOut)
    {
        // Notes sent under the old setting end now so nothing hangs at the host.
        releaseHost (hostCount, 0, toHost);
        releaseHost (clipHostCount, 0, toHost);
        prevMidiOut = midiOut;
    }
    if (jump)
    {
        clip.resync (clk.beat0, clipBank, bpm);
        arp.resync (clk.beat0, arpBank);
    }
    if (! arpOn && prevArpOn)
    {
        arp.flush (clk, arpOut);
        arp.stop();
        for (auto& r : arpRoute) for (auto& x : r) x = -1;
    }
    if (! clipOn && prevClipOn) clip.flush (clk, clipOut);
    if (clipOn && ! prevClipOn && ! first) clip.resync (clk.beat0, clipBank, bpm);
    prevArpOn = arpOn;
    prevClipOn = clipOn;
    first = false;

    // ---- UI commands (at the block start) ----------------------------------
    SeqCommand cmd;
    while (commands.pop (cmd))
    {
        const int s = std::clamp ((int) cmd.slot, 0, kNumSeqSlots - 1);
        switch (cmd.type)
        {
            case SeqCommand::ArpLaunch:   arp.launch (s, clk.beat0, arpBank); break;
            case SeqCommand::ArpStop:     arp.stop(); break;
            case SeqCommand::ClipLaunch:  clip.launch (s, clk.beat0, clipBank, bpm); break;
            case SeqCommand::ClipStop:    clip.stopSlot (s, clk.beat0); break;
            case SeqCommand::ClipToggle:  clip.toggle (s, clk.beat0, clipBank, bpm); break;
            case SeqCommand::ClipStopAll: clip.stopAll (clk.beat0); break;
            case SeqCommand::RecordStart: clip.startRecording (s, clipBank.recordMode, clk.beat0, clipBank, bpm); break;
            case SeqCommand::RecordStop:  clip.stopRecording (clk.beat0, clipBank, bpm); break;
            case SeqCommand::AllNotesOff:
            default:                      allNotesOff (0, toVoices, toHost); break;
        }
    }

    // ---- 1. split the input ------------------------------------------------
    auto inOctave = [] (int note, int oct) { return oct >= 0 && note >= oct * 12 && note < oct * 12 + 12; };
    for (int i = 0; i < numIn; ++i)
    {
        MidiEvent e = in[i];
        e.offset = std::clamp (e.offset, 0, numSamples - 1);
        e.source = MidiEvent::kInput;
        const double beat = clk.beatAt (e.offset);
        const int p = e.note();
        if (e.isNoteOn())
        {
            if (clipOn && inOctave (p, clipBank.triggerOctave))
            {
                consumed[p] = kClipTrigger;
                clip.launch (p % 12, beat, clipBank, bpm, p, e.velocity());
                continue;
            }
            if (arpOn && inOctave (p, arpBank.triggerOctave))
            {
                consumed[p] = kArpTrigger;
                arpIn.add ({ e.offset, kArpLaunchStatus, (uint8_t) (p % 12), 0, MidiEvent::kInput });
                continue;
            }
            if (clipOn)
            {
                const int ks = kbSlot (clipBank);
                const Clip& c = clipBank.clips[(size_t) ks];
                if (c.settings.kbSpan != 0)
                {
                    consumed[p] = kClipKeys;
                    if (c.settings.kbSpan == 3)
                    {
                        const double step = offsetQuantBeats (c.offsetQuant) > 0.0 ? offsetQuantBeats (c.offsetQuant) : 0.25;
                        clip.launch (ks, beat, clipBank, bpm, p, e.velocity(), 0, (p - 60) * step, false);
                    }
                    else clip.launch (ks, beat, clipBank, bpm, p, e.velocity(), p - 60, 0.0, c.settings.kbSpan == 2);
                    continue;
                }
            }
            consumed[p] = kPlay;
            if (clip.isRecording()) clip.recordNoteOn (p, e.velocity(), beat, clipBank, bpm);
            playable.add (e);
        }
        else if (e.isNoteOff())
        {
            const Consumed c = consumed[p];
            consumed[p] = kNone;
            if (c == kClipTrigger)
            {
                if (clipBank.clips[(size_t) (p % 12)].settings.noteGate) clip.stopKey (p, beat);
                continue;
            }
            if (c == kClipKeys)
            {
                const int ks = kbSlot (clipBank);
                if (clipBank.clips[(size_t) ks].settings.noteGate) clip.stopKey (p, beat);
                continue;
            }
            if (c == kArpTrigger) continue;
            if (clip.isRecording()) clip.recordNoteOff (p, e.velocity(), beat, clipBank, bpm);
            playable.add (e);
        }
        else if (arpOn && e.isCC() && e.ccNumber() == 64) arpIn.add (e);    // sustain drives LATCH
        else playable.add (e);
    }

    // ---- 2. clip player -----------------------------------------------------
    if (clipOn) clip.render (clk, clipBank, bpm, swing, clipOut);

    // ---- 3. arpeggiator -----------------------------------------------------
    arpIn.addAll (playable);
    arpIn.addAll (clipOut);
    if (arpOn)
    {
        const int kLo = arpKeyLo.load (std::memory_order_relaxed), kHi = arpKeyHi.load (std::memory_order_relaxed);
        NoteRange range;
        range.keyLo = kLo; range.keyHi = kHi;
        range.velLo = arpVelLo.load (std::memory_order_relaxed); range.velHi = arpVelHi.load (std::memory_order_relaxed);
        range.fold = arpFold.load (std::memory_order_relaxed);
        const bool thru = arpBank.slots[(size_t) arp.slotIndex (arpBank)].thru;
        for (const auto& e : arpIn)
        {
            arp.advance (clk, e.offset, arpBank, bpm, swing);
            const double beat = clk.beatAt (e.offset);
            if (e.status == kArpLaunchStatus) { arp.launch (e.data1, beat, arpBank); continue; }
            if (e.isCC() && e.ccNumber() == 64 && e.source == MidiEvent::kInput) { arp.setSustain (e.ccValue() >= 64, arpBank); continue; }
            const int src = std::min<int> (e.source, 2);
            if (e.isNoteOn())
            {
                const int m = range.map (e.note(), e.velocity());
                arpRoute[src][e.note()] = (int16_t) m;
                if (m < 0) { direct.add (e); continue; }
                arp.noteOn (m, e.velocity(), beat, arpBank);
                if (thru) direct.add (e);
            }
            else if (e.isNoteOff())
            {
                const int m = arpRoute[src][e.note()];
                arpRoute[src][e.note()] = -1;
                if (m >= 0) arp.noteOff (m, beat, arpBank);
                direct.add (e);         // ends a THRU / bypassed / pre-arp note (ignored if none)
            }
            else direct.add (e);
        }
        arp.advance (clk, numSamples, arpBank, bpm, swing);
        arp.emit (clk, arpOut);
        finalStream.addAll (arpOut);
        finalStream.addAll (direct);
    }
    else
    {
        finalStream.addAll (arpOut);    // flush output when the arp was just switched off
        finalStream.addAll (arpIn);
    }

    // ---- 4. transpose + key/scale, MIDI OUT --------------------------------
    for (const auto& e : finalStream)
        mapEvent (e, transpose, key, scale, midiOut == 2, toVoices, toHost);

    if (midiOut == 1)
        for (const auto& e : clipOut)
        {
            if (e.isNoteOn()) trackOn (clipHostCount, e.note(), e.velocity(), e.offset, chan, MidiEvent::kClip, toHost);
            else if (e.isNoteOff()) trackOff (clipHostCount, e.note(), e.velocity(), e.offset, chan, MidiEvent::kClip, toHost);
        }

    publish (arpBank, toVoices);
}

void NoteProcessor::publish (const ArpBank& arpBank, const MidiBuffer& toVoices) noexcept
{
    const auto r = std::memory_order_relaxed;
    telem.arpSlot.store (arp.playingSlot (arpBank), r);
    telem.arpStep.store (arp.stepIndex(), r);
    uint32_t playing = 0, pending = 0;
    for (int s = 0; s < kNumSeqSlots; ++s)
    {
        if (clip.isPlaying (s)) playing |= 1u << s;
        if (clip.isPending (s)) pending |= 1u << s;
        telem.clipPlayhead[s].store ((float) clip.playhead (s), r);
    }
    telem.clipPlaying.store (playing, r);
    telem.clipPending.store (pending, r);
    telem.recordingSlot.store (clip.recordingSlot(), r);
    telem.recordPosition.store ((float) clip.recordPosition(), r);
    telem.beat.store (expectedBeat, r);
    uint32_t n = telem.noteCount.load (r);
    for (const auto& e : toVoices)
        if (e.isNoteOn())
        {
            telem.notes[n % SeqTelemetry::kHistory].store ((uint32_t) e.note() | ((uint32_t) e.velocity() << 8) | ((uint32_t) e.source << 16), r);
            ++n;
        }
    telem.noteCount.store (n, std::memory_order_release);
}

int NoteProcessor::drainRecorded (ClipBank& bank)
{
    int added = 0;
    RecordedNote r;
    while (clip.popRecorded (r))
    {
        Clip& c = bank.clips[(size_t) std::clamp (r.slot, 0, kNumSeqSlots - 1)];
        ClipNote n;
        n.start = r.start;
        n.length = r.length;
        n.pitch = r.pitch;
        n.velocity = std::max<int> (1, r.velocity);
        n.releaseVelocity = r.releaseVelocity;
        n.take = 1;
        c.addNote (n);
        if (bank.recordMode == 1) c.length = std::max (c.length, std::ceil ((n.end() - 1.0e-9) / 4.0) * 4.0);
        ++added;
    }
    const int rs = telem.recordingSlot.load (std::memory_order_relaxed);
    if (bank.recordMode == 1 && rs >= 0 && rs < kNumSeqSlots)
    {
        Clip& c = bank.clips[(size_t) rs];
        c.length = std::max (c.length, std::ceil (telem.recordPosition.load (std::memory_order_relaxed) / 4.0) * 4.0);
    }
    return added;
}
} // namespace tincture
