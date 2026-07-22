#pragma once

// The AU v2 effect adapter wraps Apple's AudioUnitSDK (Apple-only,
// developer-supplied). The whole header is gated on __APPLE__ so it stays
// self-contained — an empty no-op — on the Linux header-hygiene check and any
// non-Apple TU.
#if defined(__APPLE__)

#include <AudioUnitSDK/AUMIDIEffectBase.h>
#include <AudioToolbox/AudioUnitProperties.h>  // AUMIDIOutputCallbackStruct

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/adapter_boundary.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace pulp::format::au {

/// Maximum number of `AUChannelInfo` pairs `build_channel_info` can produce.
/// Pulp's flexible channel contract allows mono / stereo per main bus
/// (`validate_channel_layout` constrains every count to {1, 2}), so the worst
/// case is two pairs (mono and stereo). Sized as a small constant so callers can
/// back the array with fixed storage and avoid heap allocation.
inline constexpr std::size_t kMaxChannelInfoPairs = 16;

/// Derive the AU `kAudioUnitProperty_SupportedNumChannels` table from a Pulp
/// `PluginDescriptor`. Surfaces which (input, output) channel-count pairs the
/// plugin handles so hosts and auval can negotiate a layout instead of guessing.
///
/// Reports the channel counts the descriptor ACTUALLY declares (clamped into the
/// supported `{1, 2}` flex range per `validate_channel_layout`), so asymmetric
/// configs are not hidden from the host:
///   * Instrument / generator with NO input bus (0 input channels): report
///     `inChannels = 0` paired with each supported output width up to the
///     declared maximum — `{0,1}` for a mono synth, `{0,1}` and `{0,2}` for a
///     stereo synth.
///   * Symmetric effect (declared input width == output width): report the
///     matching `in == out` flex ladder up to that width — `{1,1}` for mono,
///     `{1,1}` and `{2,2}` for stereo. Mono-down flex stays available so a host
///     can run a stereo plugin on a mono track.
///   * Asymmetric effect (declared input width != output width, e.g. a
///     mono-in / stereo-out widener): report the single exact declared pair,
///     e.g. `{1,2}`. Hiding it behind a matched ladder would mis-report the
///     plugin's real capability.
///
/// Writes at most `kMaxChannelInfoPairs` entries into `out` and returns the
/// count. `out` must point to storage for at least `kMaxChannelInfoPairs`
/// entries. Pure / allocation-free so the AU `SupportedNumChannels` override can
/// fill caller-owned storage.
inline UInt32 build_channel_info(const PluginDescriptor& desc,
                                 AUChannelInfo* out) noexcept {
    if (!desc.supported_bus_layouts.empty()) {
        UInt32 count = 0;
        for (const auto& layout : desc.supported_bus_layouts) {
            const int in_w = layout.inputs.empty() ? 0 : layout.inputs.front();
            const int out_w = layout.outputs.empty() ? 0 : layout.outputs.front();
            if (in_w < 0 || out_w <= 0 || in_w > std::numeric_limits<SInt16>::max() ||
                out_w > std::numeric_limits<SInt16>::max()) continue;
            bool duplicate = false;
            for (UInt32 i = 0; i < count; ++i) {
                duplicate |= out[i].inChannels == in_w && out[i].outChannels == out_w;
            }
            if (duplicate) continue;
            if (count == kMaxChannelInfoPairs) break;
            out[count++] = {static_cast<SInt16>(in_w), static_cast<SInt16>(out_w)};
        }
        return count;
    }
    // Clamp the declared widths into the supported {1, 2} flex range; a declared
    // width of 0 means "no bus" (instrument input) and stays 0.
    auto clamp_width = [](int n) -> int {
        if (n <= 0) return 0;
        return n >= 2 ? 2 : 1;
    };
    const int in_w = clamp_width(desc.default_input_channels());
    const int out_w = clamp_width(desc.default_output_channels());

    UInt32 count = 0;
    if (in_w == 0) {
        // No input bus (instrument / generator): 0 in, each output width up to
        // the declared maximum.
        for (int w = 1; w <= out_w && count < kMaxChannelInfoPairs; ++w) {
            out[count].inChannels = 0;
            out[count].outChannels = static_cast<SInt16>(w);
            ++count;
        }
    } else if (in_w == out_w) {
        // Symmetric effect: matching in==out pairs up to the declared width.
        for (int w = 1; w <= in_w && count < kMaxChannelInfoPairs; ++w) {
            out[count].inChannels = static_cast<SInt16>(w);
            out[count].outChannels = static_cast<SInt16>(w);
            ++count;
        }
    } else {
        // Asymmetric effect: report the exact declared pair (e.g. {1,2}).
        out[count].inChannels = static_cast<SInt16>(in_w);
        out[count].outChannels = static_cast<SInt16>(out_w);
        ++count;
    }
    return count;
}

/// Fill `ProcessBusBufferInfo` for an AU v2 EFFECT's input buses: the main input
/// (index 0, role Main) plus — when the descriptor declares a second input bus —
/// a Sidechain bus (index 1, role Sidechain).
///
/// AUEffectBase is a single-input/single-output kernel model (`AUBase(ci, 1, 1)`,
/// and `AUEffectBase::Render` pulls ONLY input element 0), so a Pulp AU effect
/// cannot receive live sidechain audio through the stock render path. The
/// sidechain view is therefore emitted INACTIVE: `Processor::sidechain_input()`
/// returns `nullptr` gracefully (a disconnected bus delivering null without
/// reordering the bus->buffer mapping) rather than exposing uninitialised memory
/// or a bus the host would expect to feed. Live AU-effect sidechain delivery is a
/// documented AU v2 limitation (the instrument/aumu path carries real multi-bus).
///
/// Pure / allocation-free and unit-testable without an `AudioComponentInstance`.
/// Names view into `desc`, which must outlive the filled infos. The adapter sets
/// each view's `.active` from the live host channel count before process().
inline std::size_t build_input_bus_infos(const PluginDescriptor& desc,
                                         ProcessBusBufferInfo* out,
                                         std::size_t cap) noexcept {
    if (cap == 0) return 0;
    std::size_t n = 0;
    out[n].name = desc.input_buses.empty()
                      ? std::string_view{"Audio In"}
                      : std::string_view{desc.input_buses[0].name};
    out[n].index = 0;
    out[n].direction = BusDirection::Input;
    out[n].role = BusRole::Main;
    out[n].declared_channels = desc.default_input_channels();
    out[n].optional = desc.default_input_channels() == 0;
    out[n].active = true;  // adapter overwrites from live channel count
    ++n;
    if (desc.input_buses.size() > 1 && n < cap) {
        out[n].name = desc.input_buses[1].name;
        out[n].index = 1;
        out[n].direction = BusDirection::Input;
        out[n].role = BusRole::Sidechain;
        out[n].declared_channels = desc.input_buses[1].default_channels;
        out[n].optional = true;
        out[n].active = false;  // AUEffectBase does not pull a 2nd input element
        ++n;
    }
    return n;
}

class PulpAUEffect : public ausdk::AUMIDIEffectBase {
public:
    explicit PulpAUEffect(AudioComponentInstance ci);
    // Multi-plugin-bundle ctor: the component is bound to its own factory
    // lexically (no global registry lookup). The single-arg ctor delegates
    // here with the legacy `registered_factory()` for single-plugin bundles.
    PulpAUEffect(AudioComponentInstance ci, ProcessorFactory factory);

    OSStatus GetParameterList(AudioUnitScope inScope,
                              AudioUnitParameterID* outParameterList,
                              UInt32& outNumParameters) override;
    OSStatus GetParameterInfo(AudioUnitScope inScope,
                              AudioUnitParameterID inParameterID,
                              AudioUnitParameterInfo& outParameterInfo) override;
    OSStatus GetParameterValueStrings(AudioUnitScope inScope,
                                      AudioUnitParameterID inParameterID,
                                      CFArrayRef* outStrings) override;

    // Single-source-of-truth parameter access. The plugin's
    // StateStore IS the parameter store: the host reads it via GetParameter and
    // writes it via SetParameter, so there is no separate AUElement/Globals
    // copy to reconcile each block (that split caused the UI snap-back / render
    // stalls). process() reads the same store; UI edits notify the host via a
    // main-thread listener. See the au_v2_adapter.cpp definitions.
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

    /// Surface the plugin's supported (input, output) channel-count pairs so the
    /// host (and auval) can negotiate a layout. Derived from the descriptor via
    /// `build_channel_info`. Returns the pair count; fills a per-instance static
    /// table when `outInfo` is non-null. 0 from the base class meant "property
    /// unsupported", which left every channel-config query unanswered.
    UInt32 SupportedNumChannels(const AUChannelInfo** outInfo) override;

    OSStatus Initialize() override;
    void Cleanup() override;

    OSStatus ProcessBufferLists(AudioUnitRenderActionFlags& ioActionFlags,
                                const AudioBufferList& inBuffer,
                                AudioBufferList& outBuffer,
                                UInt32 inFramesToProcess) override;

    OSStatus SaveState(CFPropertyListRef* outData) override;
    OSStatus RestoreState(CFPropertyListRef plist) override;

    bool SupportsTail() override;
    Float64 GetTailTime() override;
    Float64 GetLatency() override;

protected:
    /// Called by the AU host for every short MIDI message (status has
    /// already been split into ``inStatus`` / ``inChannel``). Converts the
    /// bytes to a ``midi::MidiEvent`` and pushes it onto the lock-free
    /// render queue. Drained in ``ProcessBufferLists``.
    ///
    /// Note: DAW hosts only route MIDI to an AU v2 effect when the
    /// bundle's component ``type`` is ``aumf`` (kAudioUnitType_MusicEffect).
    /// Plug-ins packaged as ``aufx`` still have this override wired — the
    /// host just never calls it — so leaving the path here for ``aufx``
    /// plug-ins is harmless.
    // NB: AudioUnitSDK declares these as `AUSDK_RTSAFE` (which expands to
    // `[[clang::nonblocking]]`). Propagating that attribute into an
    // `override` declaration compiles under older Xcode but Xcode 16.4 /
    // Clang 17+ rejects the attribute position with
    // "expected ';' at end of declaration list". The attribute is a
    // static-analysis hint only — dropping it has no runtime effect, and
    // matches the pattern used by `PulpAUInstrument::HandleNoteOn/Off`.
    OSStatus HandleMIDIEvent(UInt8 inStatus, UInt8 inChannel,
                             UInt8 inData1, UInt8 inData2,
                             UInt32 inStartFrame) override;

    /// System-exclusive payload (F0 … F7). ``AUMIDIBase::HandleSysEx`` does
    /// not carry a per-event sample offset at this SDK layer — we enqueue
    /// the sysex with ``sample_offset == 0`` so it is delivered at the
    /// block boundary.
    OSStatus HandleSysEx(const UInt8* inData, UInt32 inLength) override;

private:
    /// True when the wrapped Processor's descriptor declares `produces_midi`.
    /// Gates the MIDI-output property surface so plain audio effects never
    /// advertise a MIDI output the host would try to wire up.
    bool plugin_produces_midi() const noexcept;

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
    // path can view its bus-name strings without copying the descriptor per
    // block (which would allocate std::string members on the audio thread).
    PluginDescriptor descriptor_{};

    // Main-thread listener that pushes editor parameter edits to the host
    // (AudioUnitSetParameter), kept alive for the adapter's lifetime so host
    // param writes/notifications never run on the render thread. See ctor.
    state::ListenerToken ui_push_listener_;

    // Parameter-event sidecar, set on the Processor each block so the
    // param-events contract is uniform across formats. AU v2's AUEffectBase
    // has no scheduled/ramped parameter event source today, so this queue is
    // empty: host parameter changes still reach the Processor through `store_`
    // exactly as before.
    state::ParameterEventQueue param_events_;

    // Per-block MIDI I/O. Hoisted to members (not constructed per render call)
    // and given reserved, realtime-capacity-limited storage in Initialize() so
    // the drain loop's add()/add_sysex_copy() never grows a vector on the audio
    // thread. Reset with clear()+clear_sysex() each block. Capacities match the
    // VST3 adapter so behavior is uniform across formats.
    static constexpr std::size_t kMaxEventsPerBlock = kMaxMidiEventsPerBlock;
    static constexpr std::size_t kMaxSysexPerBlock = 64;
    static constexpr std::size_t kMaxSysexPayloadBytes = 512;
    midi::MidiBuffer midi_in_;
    midi::MidiBuffer midi_out_;

    // Host accommodations, resolved once in the constructor via the
    // runtime policy.
    HostQuirks host_quirks_{};

    // Cached ParamID of the "Bypass" parameter (plugin-declared or
    // synthesized by host-quirk policy). 0 when none is available, so
    // ProcessBufferLists never short-circuits to pass-through.
    state::ParamID bypass_param_id_ = 0;
    // Dry-delay line for the bypass pass-through. Sized to the processor's
    // reported latency in Initialize(); keeps the bypassed dry signal aligned
    // with the host's plugin-delay-compensation instead of arriving `latency`
    // samples early (a zero latency leaves it a no-op zero-copy passthrough).
    boundary::LatencyCompensatedBypass bypass_;
    std::vector<const float*> input_ptrs_;
    std::vector<float*> output_ptrs_;

    // Previous-block transport snapshot used to derive change flags on
    // `ProcessContext`. Default-constructed so the first process() call
    // after init reports no changes.
    detail::PlayheadSnapshot playhead_prev_{};

    // MIDI input path — AU v2 effects that declare accepts_midi are packaged as
    // aumf (kAudioUnitType_MusicEffect). The host routes inbound MIDI through
    // AUMIDIBase::MIDIEvent / SysEx → HandleMIDIEvent / HandleSysEx. Those are
    // LOCK-FREE single-producer (host MIDI/render thread) queues drained by the
    // single consumer (ProcessBufferLists) — no mutex on the audio thread, the
    // same atomic/wait-free discipline as the parameter store. Short messages
    // are allocation-free; under flood, excess events are dropped rather than
    // blocking the render (lossy, like the RT parameter notify path). aufx
    // effects never receive MIDI, so the queues stay empty.
    runtime::SpscQueue<midi::MidiEvent, 1024> midi_in_queue_;
    struct SysexChunk {
        std::array<uint8_t, 512> bytes{};
        uint16_t length = 0;
    };
    runtime::SpscQueue<SysexChunk, 32> sysex_in_queue_;

    // MIDI OUTPUT path. A plugin that declares `produces_midi` (aumf MIDI effect
    // or an instrument routed back to MIDI) emits events into midi_out_ during
    // process(). AU v2 delivers them through kAudioUnitProperty_MIDIOutputCallback:
    // the host writes a callback (SetProperty) and reads the output name list
    // (kAudioUnitProperty_MIDIOutputCallbackInfo). The render path packs midi_out_
    // into a MIDIPacketList and calls the host callback. The publisher owns the
    // main-thread → render-thread handoff of the (callback, userData) pair.
    MidiOutputCallbackPublisher midi_output_callback_;
    // Pre-reserved packet-list storage so the render-path build never allocates.
    MidiOutputPacketBuilder midi_out_packet_builder_;

    // Per-instance channel-config table filled by SupportedNumChannels. Member
    // (not call-local) so the pointer handed to the host outlives the call.
    std::array<AUChannelInfo, kMaxChannelInfoPairs> channel_info_{};
};

} // namespace pulp::format::au

#endif // defined(__APPLE__)
