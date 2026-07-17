#pragma once

// The AU v2 effect adapter wraps Apple's AudioUnitSDK (Apple-only,
// developer-supplied). The whole header is gated on __APPLE__ so it stays
// self-contained — an empty no-op — on the Linux header-hygiene check and any
// non-Apple TU.
#if defined(__APPLE__)

#include <AudioUnitSDK/AUMIDIEffectBase.h>
#include <AudioToolbox/AudioUnitProperties.h>  // AUMIDIOutputCallbackStruct
#include <CoreMIDI/MIDIServices.h>  // MIDIPacketList builders for MIDI output
#include <mach/mach_time.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/midi_out_offset.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
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

/// Custom AU property ID the Cocoa view factory queries to obtain
/// the host-side Processor + StateStore pointers. Fixes the former
/// "dual-Processor" bug where the Cocoa view created a second Processor
/// instance that silently desynchronized from the host's audio-thread
/// Processor. Property is Global scope, read-only.
static constexpr AudioUnitPropertyID kPulpEditorContextProperty = 0x50754564; // 'PuEd'

struct PulpEditorContext {
    pulp::format::Processor* processor = nullptr;
    pulp::state::StateStore* store = nullptr;
    pulp::runtime::AliveToken::Handle owner_alive;
};

/// Cross-TU Cocoa-view hook. The AU adapter classes (`PulpAUEffect`,
/// `PulpAUInstrument`) are compiled into the shared `pulp-format` library
/// WITHOUT `PULP_AU_GUI`, while the Cocoa view factory + its
/// `AudioUnitCocoaViewInfo` filler live in `au_v2_cocoa_view.mm`, added
/// per-plugin to the `*_AU` target WITH `PULP_AU_GUI`. A compile-time
/// `#ifdef` in the adapters would therefore always be off. Instead, the GUI
/// module registers its filler here at static-init, and the adapters read it
/// (ungated) from `GetProperty(kAudioUnitProperty_CocoaUI)`.
///
/// Null when no GUI module is linked (CLAP / Standalone / headless tests) —
/// the adapters then report no Cocoa view and the host uses its generic view.
/// Without this hook the host is NEVER told the plugin has a custom editor —
/// the bug ChainerSynth surfaced (the editor never showed in Logic regardless
/// of the GPU-host wiring; only AU effects had ANY editor-context property and
/// none advertised the Cocoa view at all).
using CocoaViewInfoFiller = bool (*)(void*);
extern CocoaViewInfoFiller g_cocoa_view_info_filler;

/// Fills an `AudioUnitCocoaViewInfo` with the Pulp Cocoa view factory's bundle
/// URL + class name. Defined in `au_v2_cocoa_view.mm` (PULP_AU_GUI); installed
/// into `g_cocoa_view_info_filler` at static-init. Returns false if the factory
/// class isn't available. `outData` must point to an `AudioUnitCocoaViewInfo`.
bool fill_cocoa_view_info(void* outData);

/// Decode a short MIDI status byte and two data bytes into a
/// ``midi::MidiEvent``. Exposed at namespace scope so the AU-MIDI routing
/// can be unit tested without constructing a real ``AudioComponentInstance``.
///
/// Channel voice messages arrive at ``AUMIDIBase::HandleMIDIEvent`` already
/// split into ``inStatus`` (top nibble, e.g. ``0x80``, ``0xB0``) and
/// ``inChannel`` (bottom nibble, 0–15). This helper re-combines them into
/// a single status byte so the resulting ``choc::midi::ShortMessage`` has a
/// consistent on-the-wire layout regardless of whether the caller already
/// split the channel out.
///
/// @param inStatus  Status byte (top nibble, bottom nibble optional).
/// @param inChannel MIDI channel (0–15). Ignored for system messages.
/// @param inData1   Data byte 1 (note number, CC number, etc.).
/// @param inData2   Data byte 2 (velocity, CC value, etc.).
/// @returns MidiEvent with ``sample_offset == 0`` — callers should set
///          the sample offset themselves before enqueuing.
midi::MidiEvent decode_midi_event(uint8_t inStatus,
                                  uint8_t inChannel,
                                  uint8_t inData1,
                                  uint8_t inData2) noexcept;

/// Build the AU v2 render-path ProcessContext fields that are independent of
/// host callbacks. AU v2 render callbacks are always realtime; offline bounce
/// intent is not surfaced by the v2 SDK, so hosts that need explicit offline
/// hints should use AU v3 or another adapter that provides that signal.
inline ProcessContext make_render_process_context(double sample_rate,
                                                  int num_samples) noexcept {
    ProcessContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.num_samples = num_samples;
    ctx.process_mode = ProcessMode::Realtime;
    ctx.render_speed_hint = RenderSpeedHint::Realtime;
    return ctx;
}

/// Populate AU v2 render-context transport metadata from host callbacks and
/// derive per-block playhead change flags. The AU v2 SDK only exposes these
/// callbacks through AUBase during render, but keeping the mapping here lets
/// effect/instrument adapters share one contract and lets tests install
/// HostCallbackInfo without constructing a full AudioComponentInstance.
inline void apply_host_callbacks_to_process_context(
    ProcessContext& ctx,
    const ausdk::AUBase& unit,
    detail::PlayheadSnapshot& previous) noexcept {
    Float64 beat = 0.0;
    Float64 tempo = 0.0;
    if (unit.CallHostBeatAndTempo(&beat, &tempo) == noErr) {
        ctx.position_beats = beat;
        if (tempo > 0.0) ctx.tempo_bpm = tempo;
    }

    UInt32 delta_samples = 0;
    Float32 ts_num = 0.0f;
    UInt32 ts_denom = 0;
    Float64 current_measure_downbeat = 0.0;
    if (unit.CallHostMusicalTimeLocation(&delta_samples, &ts_num, &ts_denom,
                                         &current_measure_downbeat) == noErr) {
        if (ts_num > 0.0f) ctx.time_sig_numerator = static_cast<int>(ts_num);
        if (ts_denom > 0) ctx.time_sig_denominator = static_cast<int>(ts_denom);
    }

    Boolean is_playing = false;
    Boolean transport_state_changed = false;
    Float64 current_sample_in_timeline = 0.0;
    Boolean is_cycling = false;
    Float64 cycle_start = 0.0;
    Float64 cycle_end = 0.0;
    if (unit.CallHostTransportState(&is_playing, &transport_state_changed,
                                    &current_sample_in_timeline, &is_cycling,
                                    &cycle_start, &cycle_end) == noErr) {
        ctx.is_playing = (is_playing != 0);
        ctx.position_samples = static_cast<int64_t>(current_sample_in_timeline);
        ctx.is_looping = (is_cycling != 0);
        if (ctx.is_looping) {
            ctx.loop_start_beats = cycle_start;
            ctx.loop_end_beats = cycle_end;
        }
    }

    static mach_timebase_info_data_t timebase = {0, 0};
    if (timebase.denom == 0) mach_timebase_info(&timebase);
    if (timebase.denom != 0) {
        const uint64_t now = mach_absolute_time();
        ctx.host_time_ns = static_cast<int64_t>(
            (now * timebase.numer) / timebase.denom);
    }

    detail::derive_bar_from_beats(ctx);
    detail::compute_playhead_changes(ctx, previous);
}

inline Float64 tail_samples_to_seconds(int tail_samples,
                                       double sample_rate) noexcept {
    if (tail_samples < 0) return std::numeric_limits<Float64>::infinity();
    if (tail_samples == 0 || sample_rate <= 0.0) return 0.0;
    return static_cast<Float64>(tail_samples) / sample_rate;
}

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

/// RT-safe builder for the `MIDIPacketList` an AU v2 plugin hands the host via
/// `kAudioUnitProperty_MIDIOutputCallback`. Backed by a fixed-size byte buffer
/// so building the list never allocates on the audio thread — the same
/// pre-reserved-storage discipline the MIDI input queues use.
///
/// Short channel-voice / system messages (1–3 bytes) and the Processor's output
/// SysEx are merged into a single ascending-`sample_offset` order before being
/// appended via the CoreMIDI packet-list builders (which write into the caller's
/// buffer). CoreMIDI packet lists are expected time-ordered, so the merge
/// guarantees a SysEx at offset 0 is delivered before a note at offset 64 even
/// though they live in separate sidecars. Each packet carries the Processor's
/// per-event `sample_offset`, clamped to the current block, as its timestamp —
/// the offset semantics `kAudioUnitProperty_MIDIOutputCallback` documents.
struct MidiOutputPacketBuilder {
    // Bound the per-block output by the same event budget the MIDI input path
    // reserves, so a dense arp/generator block is delivered in full rather than
    // silently truncated. Short messages are at most 3 wire bytes; the CoreMIDI
    // packet header + per-packet overhead is small, so a budget of
    // (events * ~16 bytes) comfortably covers the worst case. SysEx shares the
    // same buffer and yields to the byte bound. Events past the buffer bound are
    // dropped (counted in `dropped`) rather than overflowing.
    static constexpr std::size_t kMaxOutputEvents = 2048;  // == kMaxEventsPerBlock
    static constexpr std::size_t kBufferBytes = kMaxOutputEvents * 16;
    alignas(MIDIPacketList) std::array<std::uint8_t, kBufferBytes> storage{};

    // Number of events dropped on the most recent build() because the packet
    // buffer or the merge index filled. 0 in steady state; a diagnostic the
    // adapter / tests can read to detect a too-dense block.
    std::size_t dropped = 0;

    /// Build a `MIDIPacketList` from `midi_out` into the internal buffer.
    /// `frame_count` is the current block size; per-event offsets are clamped to
    /// `[0, frame_count - 1]` so a stray out-of-block offset (mirroring the
    /// AU v3 input-side defensive clamp) never lands a packet past the block.
    /// Returns the packet list (pointing into `storage`) when at least one packet
    /// was written, or nullptr when there is nothing to send. The returned
    /// pointer is valid until the next `build` call or destruction.
    /// Allocation-free.
    const MIDIPacketList* build(const midi::MidiBuffer& midi_out,
                                std::uint32_t frame_count) noexcept {
        dropped = 0;

        // Build a merge index over both sources (short events + sysex) so we can
        // emit in ascending sample_offset order. Each source is referenced by a
        // tagged index; the index array is fixed-capacity (no allocation).
        struct Ref {
            std::int32_t offset;
            std::uint32_t order;  // stable tiebreak: preserves per-source order
            std::uint32_t index;  // index within its source
            bool is_sysex;
        };
        std::array<Ref, kMaxOutputEvents> refs;
        std::size_t n_refs = 0;
        std::uint32_t order = 0;

        const auto clamp_offset = [&](std::int32_t off) -> std::int32_t {
            // Shared cross-format offset contract (see detail/midi_out_offset.hpp
            // and test_midi_out_offset_parity.cpp): an in-block offset N is
            // preserved; a stray out-of-block offset is clamped into the block.
            return detail::au_output_offset(off, frame_count);
        };

        std::uint32_t ev_index = 0;
        for (const auto& e : midi_out) {
            const std::uint32_t len = e.message.length();
            if (len != 0 && len <= 3) {  // short messages only
                if (n_refs < refs.size()) {
                    refs[n_refs++] = Ref{clamp_offset(e.sample_offset),
                                         order++, ev_index, false};
                } else {
                    ++dropped;
                }
            }
            ++ev_index;
        }
        const auto& sysex = midi_out.sysex();
        for (std::uint32_t i = 0; i < sysex.size(); ++i) {
            if (sysex[i].data.empty()) continue;
            if (n_refs < refs.size()) {
                refs[n_refs++] = Ref{clamp_offset(sysex[i].sample_offset),
                                     order++, i, true};
            } else {
                ++dropped;
            }
        }

        // Stable sort by offset (insertion sort — small n, no allocation, and
        // stable so same-offset events keep per-source emission order).
        for (std::size_t i = 1; i < n_refs; ++i) {
            Ref key = refs[i];
            std::size_t j = i;
            while (j > 0 && (refs[j - 1].offset > key.offset ||
                             (refs[j - 1].offset == key.offset &&
                              refs[j - 1].order > key.order))) {
                refs[j] = refs[j - 1];
                --j;
            }
            refs[j] = key;
        }

        auto* list = reinterpret_cast<MIDIPacketList*>(storage.data());
        MIDIPacket* cur = MIDIPacketListInit(list);
        bool wrote = false;

        for (std::size_t i = 0; i < n_refs; ++i) {
            const Ref& r = refs[i];
            const auto ts = static_cast<MIDITimeStamp>(r.offset);
            MIDIPacket* next = nullptr;
            if (r.is_sysex) {
                const auto& sx = sysex[r.index];
                next = MIDIPacketListAdd(list, kBufferBytes, cur, ts,
                                         sx.data.size(), sx.data.data());
            } else {
                // Random-access the short event by its buffer index.
                const midi::MidiEvent& e = midi_out[r.index];
                const std::uint32_t len = e.message.length();
                std::uint8_t bytes[3] = {0, 0, 0};
                const auto* d = e.data();
                for (std::uint32_t b = 0; b < len; ++b) bytes[b] = d[b];
                next = MIDIPacketListAdd(list, kBufferBytes, cur, ts, len, bytes);
            }
            if (next == nullptr) {  // buffer full — drop the rest
                dropped += (n_refs - i);
                break;
            }
            cur = next;
            wrote = true;
        }

        return wrote ? list : nullptr;
    }
};

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
    static constexpr std::size_t kMaxEventsPerBlock = 2048;
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
    // into a MIDIPacketList and calls the host callback.
    //
    // The (callback fn, userData) pair is written on the main thread (SetProperty)
    // and read on the render thread, so it MUST be published atomically — a
    // non-atomic two-pointer struct could tear and pair a fresh callback with a
    // stale userData. We double-buffer two immutable pairs and publish the active
    // one through an atomic pointer: SetProperty writes the inactive slot then
    // releases it; the render thread does a single acquire-load. The render reader
    // never dereferences a half-written pair.
    struct MidiOutputCallbackPair {
        AUMIDIOutputCallback callback = nullptr;
        void* user_data = nullptr;
    };
    std::array<MidiOutputCallbackPair, 2> midi_output_callback_slots_{};
    std::atomic<std::uint8_t> midi_output_callback_write_slot_{0};
    std::atomic<const MidiOutputCallbackPair*> midi_output_callback_{nullptr};
    // Pre-reserved packet-list storage so the render-path build never allocates.
    MidiOutputPacketBuilder midi_out_packet_builder_;

    // Per-instance channel-config table filled by SupportedNumChannels. Member
    // (not call-local) so the pointer handed to the host outlives the call.
    std::array<AUChannelInfo, kMaxChannelInfoPairs> channel_info_{};
};

} // namespace pulp::format::au

#endif // defined(__APPLE__)
