#pragma once

// The AU v2 effect adapter wraps Apple's AudioUnitSDK (Apple-only,
// developer-supplied). The whole header is gated on __APPLE__ so it stays
// self-contained — an empty no-op — on the Linux header-hygiene check and any
// non-Apple TU.
#if defined(__APPLE__)

#include <AudioUnitSDK/AUMIDIEffectBase.h>
#include <mach/mach_time.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace pulp::format::au {

/// Custom AU property ID the Cocoa view factory queries to obtain
/// the host-side Processor + StateStore pointers. Fixes the former
/// "dual-Processor" bug where the Cocoa view created a second Processor
/// instance that silently desynchronized from the host's audio-thread
/// Processor. Property is Global scope, read-only.
static constexpr AudioUnitPropertyID kPulpEditorContextProperty = 0x50754564; // 'PuEd'

struct PulpEditorContext {
    pulp::format::Processor* processor;
    pulp::state::StateStore* store;
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

class PulpAUEffect : public ausdk::AUMIDIEffectBase {
public:
    explicit PulpAUEffect(AudioComponentInstance ci);

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
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;

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
    // VST3 adapter so behaviour is uniform across formats.
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
};

} // namespace pulp::format::au

#endif // defined(__APPLE__)
