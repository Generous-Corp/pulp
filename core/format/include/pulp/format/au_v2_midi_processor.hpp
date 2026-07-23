#pragma once

#include <pulp/runtime/alive_token.hpp>

// The AU v2 MIDI-processor adapter wraps Apple's AudioUnitSDK (Apple-only,
// developer-supplied). The whole header is gated on __APPLE__ so it stays
// self-contained — an empty no-op — on the Linux header-hygiene check and any
// non-Apple TU.
#if defined(__APPLE__)

#include <AudioUnitSDK/MusicDeviceBase.h>

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace pulp::format::au {

/// AU v2 adapter for a Pulp `Processor` whose descriptor declares
/// `PluginCategory::MidiEffect` — an AU MIDI processor, component type `aumi`
/// (`kAudioUnitType_MIDIProcessor`). MIDI in, MIDI out, no audio processing.
///
/// **Base class.** `MusicDeviceBase` (`AUBase` + `AUMIDIBase`), the same base
/// the instrument adapter uses. `AUMIDIEffectBase` is not an option: it derives
/// `AUEffectBase`, which hardwires one audio input element and one audio output
/// element and pulls the input every render — a MIDI processor has no audio
/// input to pull. `MusicDeviceBase` also supplies the `MIDIEvent` / `SysEx`
/// forwarding into `AUMIDIBase` and the MIDI-mapping property delegation, which
/// a hand-rolled `AUBase` + `AUMIDIBase` pairing would have to restate.
///
/// **Entry factory.** Register through `ausdk::AUMIDIEffectFactory`
/// (`AUMIDILookup` = base selectors + `MusicDeviceMIDIEvent` + `MusicDeviceSysEx`)
/// via `PULP_AU_MIDI_EFFECT`. The class implementing `HandleMIDIEvent` is not
/// enough — a factory only dispatches the selectors its lookup table carries, so
/// the plain `AUBaseFactory` makes every host MIDI delivery return -4 (unimpErr).
/// `AUMusicDeviceFactory` would additionally carry StartNote / StopNote, which a
/// MIDI processor is not: `AUMIDILookup` is the exact selector set.
///
/// **Element shape.** Zero audio input elements and ONE audio output element.
/// The output element carries no musical signal — the render path zeroes it and
/// flags the block silent — but it has to exist: an AU v2 host advances a plugin
/// by rendering it, and rendering is what drains the inbound MIDI queue, runs
/// `process()`, and delivers the Processor's outbound MIDI through
/// `kAudioUnitProperty_MIDIOutputCallback`. With no output element there is no
/// bus for the host to pull and the plugin would never run.
///
/// **MIDI output is definitional here.** Unlike the effect adapter — which gates
/// its MIDI-output property surface on `descriptor().produces_midi` so a plain
/// audio effect never advertises a MIDI output — an `aumi` always advertises it.
/// A MIDI processor that could not emit MIDI would have no reason to exist, and
/// gating on the flag turns a forgotten `produces_midi = true` into silently
/// discarded output with no diagnostic.
class PulpAUMidiProcessor : public ausdk::MusicDeviceBase {
public:
    explicit PulpAUMidiProcessor(AudioComponentInstance ci);
    // Multi-plugin-bundle ctor: the component is bound to its own factory
    // lexically (no global registry lookup). The single-arg ctor delegates here
    // with the legacy `registered_factory()` for single-plugin bundles.
    PulpAUMidiProcessor(AudioComponentInstance ci, ProcessorFactory factory);

    OSStatus GetParameterList(AudioUnitScope inScope,
                              AudioUnitParameterID* outParameterList,
                              UInt32& outNumParameters) override;
    OSStatus GetParameterInfo(AudioUnitScope inScope,
                              AudioUnitParameterID inParameterID,
                              AudioUnitParameterInfo& outParameterInfo) override;
    OSStatus GetParameterValueStrings(AudioUnitScope inScope,
                                      AudioUnitParameterID inParameterID,
                                      CFArrayRef* outStrings) override;

    // Single source of truth: the host reads/writes the plugin's StateStore
    // directly — no separate Globals copy to reconcile per block. See the auv2
    // skill's "Parameters are single-source-of-truth" section.
    OSStatus GetParameter(AudioUnitParameterID inID, AudioUnitScope inScope,
                          AudioUnitElement inElement, Float32& outValue) override;
    OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope,
                          AudioUnitElement inElement, Float32 inValue,
                          UInt32 inBufferOffsetInFrames) override;

    OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
                             AudioUnitElement inElement, UInt32& outDataSize,
                             bool& outWritable) override;
    OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                         AudioUnitElement inElement, void* outData) override;
    OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                         AudioUnitElement inElement, const void* inData,
                         UInt32 inDataSize) override;

    OSStatus Initialize() override;
    void Cleanup() override;

    bool StreamFormatWritable(AudioUnitScope scope,
                              AudioUnitElement element) override;
    bool CanScheduleParameters() const noexcept override;

    OSStatus Render(AudioUnitRenderActionFlags& ioActionFlags,
                    const AudioTimeStamp& inTimeStamp,
                    UInt32 inNumberFrames) override;

    OSStatus SaveState(CFPropertyListRef* outData) override;
    OSStatus RestoreState(CFPropertyListRef plist) override;

    Float64 GetLatency() override;

protected:
    /// Every short MIDI message the host delivers (status already split into
    /// `inStatus` / `inChannel` by `AUMIDIBase::MIDIEvent`). Overriding here
    /// rather than the per-message `HandleNoteOn` / `HandleControlChange` hooks
    /// is deliberate: a MIDI processor sees the whole stream, and
    /// `MusicDeviceBase::HandleNoteOn` would otherwise route notes into
    /// `StartNote`, which an `aumi` does not implement.
    OSStatus HandleMIDIEvent(UInt8 inStatus, UInt8 inChannel,
                             UInt8 inData1, UInt8 inData2,
                             UInt32 inStartFrame) override;

    /// System-exclusive payload (F0 … F7). `AUMIDIBase::HandleSysEx` carries no
    /// per-event sample offset at this SDK layer, so the payload is enqueued at
    /// `sample_offset == 0` — the leading edge of the next rendered block.
    OSStatus HandleSysEx(const UInt8* inData, UInt32 inLength) override;

private:
    /// True when a declared Bypass parameter is engaged. A bypassed MIDI
    /// processor is a WIRE: inbound MIDI is copied to the output untouched, and
    /// `process()` is skipped. Dropping the stream instead would silence every
    /// instrument downstream of the bypassed slot.
    bool bypass_engaged() const noexcept;

    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store_;
    std::unique_ptr<Processor> processor_;
    // Declared after processor_ so reverse member destruction retires retained
    // editor handles before either referenced object is released.
    runtime::AliveToken owner_alive_;

    // Immutable plugin metadata, cached once in the constructor so the render
    // path never copies the descriptor (which allocates std::string members) on
    // the audio thread.
    PluginDescriptor descriptor_{};

    // Host accommodations, resolved once in the constructor via the runtime
    // policy. Gates the non-negative latency clamp.
    HostQuirks host_quirks_{};

    // Cached ParamID of a plugin-declared Bypass parameter, 0 when there is
    // none. This adapter does NOT synthesize one: an `aumi` has no
    // kAudioUnitProperty_BypassEffect (that lives on AUEffectBase), so a
    // synthesized control would appear as an ordinary parameter with no host
    // bypass semantics behind it.
    state::ParamID bypass_param_id_ = 0;

    // Main-thread listener that pushes editor parameter edits to the host, kept
    // alive for the adapter's lifetime so host param notifications never run on
    // the render thread.
    state::ListenerToken ui_push_listener_;

    // Parameter-event sidecar, set on the Processor each block so the
    // param-events contract is uniform across formats. AU v2 has no scheduled /
    // ramped parameter event source, so this queue stays empty and host
    // parameter changes reach the Processor through `store_`.
    state::ParameterEventQueue param_events_;

    // Per-block MIDI I/O. Members (not per-render locals) with reserved,
    // realtime-capacity-limited storage so the drain and the Processor's
    // appends never grow a vector on the audio thread. Capacities match the
    // effect adapter so behavior is uniform across formats.
    static constexpr std::size_t kMaxEventsPerBlock = kMaxMidiEventsPerBlock;
    static constexpr std::size_t kMaxSysexPerBlock = 64;
    static constexpr std::size_t kMaxSysexPayloadBytes = 512;
    midi::MidiBuffer midi_in_;
    midi::MidiBuffer midi_out_;

    // Lock-free MIDI input. Single producer (the host's MIDI delivery thread,
    // via HandleMIDIEvent / HandleSysEx), single consumer (Render). No mutex on
    // the audio thread; under flood, excess events are dropped rather than
    // blocking the render.
    runtime::SpscQueue<midi::MidiEvent, 1024> midi_in_queue_;
    struct SysexChunk {
        std::array<uint8_t, kMaxSysexPayloadBytes> bytes{};
        uint16_t length = 0;
    };
    runtime::SpscQueue<SysexChunk, 32> sysex_in_queue_;

    // MIDI output: the host installs a callback via
    // kAudioUnitProperty_MIDIOutputCallback (main thread) that the render path
    // reads every block. The publisher owns that cross-thread handoff.
    MidiOutputCallbackPublisher midi_output_callback_;
    // Pre-reserved packet-list storage so the render-path build never allocates.
    MidiOutputPacketBuilder midi_out_packet_builder_;

    // Previous-block transport snapshot used to derive playhead change flags.
    // Default-constructed so the first Render() after Initialize() reports no
    // changes.
    detail::PlayheadSnapshot playhead_prev_{};
};

} // namespace pulp::format::au

#endif // defined(__APPLE__)
