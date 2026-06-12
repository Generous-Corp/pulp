#pragma once

#include <AudioUnitSDK/AUMIDIEffectBase.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <memory>
#include <mutex>
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
    /// bytes to a ``midi::MidiEvent`` and pushes it onto the pending
    /// buffer under ``midi_mutex_``. Drained in ``ProcessBufferLists``.
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

    // Sample-accurate parameter-event sidecar, set on the Processor each block
    // so the param-events contract is uniform across formats (VST3/CLAP/AUv3
    // already provide it). AU v2's AUEffectBase has no scheduled/ramped
    // parameter event source today, so this queue is empty: host parameter
    // changes still reach the Processor through `store_` (StateStore) exactly as
    // before — this adds the uniform API + the RT-safety guard without changing
    // existing behaviour. Sample-accurate AU v2 param sourcing is a follow-up
    // (AUv3 has the AURenderEventParameter model).
    state::ParameterEventQueue param_events_;

    // Host accommodations, resolved once in the constructor via the
    // runtime policy (host-quirks plan, P3).
    HostQuirks host_quirks_{};

    // Cached ParamID of the "Bypass" parameter (plugin-declared or
    // synthesized via synthesize_bypass_parameter, host-quirks P3d). 0 when
    // none — ProcessBufferLists then never short-circuits to pass-through.
    state::ParamID bypass_param_id_ = 0;
    std::vector<const float*> input_ptrs_;
    std::vector<float*> output_ptrs_;
    // Pre-process snapshot of parameter values; used to diff plugin-side
    // changes back to the host's parameter system (workstream 01 slice 1.3).
    std::vector<float> param_snapshot_;

    // Item 1.3 — previous-block transport snapshot used to derive the
    // change flags (tempo_changed / time_sig_changed /
    // transport_changed) on `ProcessContext`. Default-constructed (no
    // previous block) so the first process() call after init reports
    // no changes.
    detail::PlayheadSnapshot playhead_prev_{};

    // MIDI input path — AU v2 effects that declare accepts_midi are
    // packaged as aumf (kAudioUnitType_MusicEffect). The host then
    // routes inbound MIDI through AUMIDIBase::MIDIEvent / SysEx, which
    // dispatches to HandleMIDIEvent / HandleSysEx. We stash events into
    // ``pending_midi_`` under ``midi_mutex_`` and drain them into the
    // block-local MidiBuffer at the top of ProcessBufferLists. The
    // mutex is only contended on the main/MIDI thread; the audio
    // thread grabs it once per block to swap pending_midi_ out.
    std::mutex midi_mutex_;
    midi::MidiBuffer pending_midi_;
};

} // namespace pulp::format::au
