#pragma once

// VST3 MIDI-controller → ParamID mapping helpers.
//
// VST3 has no raw MIDI CC / pitch-bend / aftertouch events. Controllers
// reach a plug-in only through IMidiMapping: the host queries which
// *parameter* each (event-bus, channel, controller) triple maps to, then
// delivers those controllers as ordinary parameter changes in
// ProcessData::inputParameterChanges. The adapter reserves a private
// ParamID range for these controllers, registers them as hidden
// parameters, and decodes any inbound parameter change whose ID falls in
// the reserved range back into a MIDI message for the Processor.
//
// This header isolates the pure arithmetic (encode / decode / range test)
// so it is unit-testable without pulling the Steinberg VST3 SDK into the
// test binary, mirroring vst3_frame_rate.hpp. The controller-number
// convention matches VST3's Vst::ControllerNumbers:
//   0..127  standard MIDI CC numbers (mod wheel = 1, sustain = 64, ...)
//   128     channel aftertouch  (Vst::kAfterTouch)
//   129     pitch bend          (Vst::kPitchBend)
// 130 controllers per channel (Vst::kCountCtrlNumber) across 16 channels.

#include <pulp/state/parameter.hpp>
#include <cstdint>

namespace pulp::format::detail {

/// Number of controller slots per MIDI channel: 128 CCs + channel
/// aftertouch + pitch bend. Matches Vst::ControllerNumbers::kCountCtrlNumber.
inline constexpr int kVst3ControllersPerChannel = 130;

/// Controller index for channel aftertouch (Vst::kAfterTouch).
inline constexpr int kVst3CtrlAfterTouch = 128;

/// Controller index for pitch bend (Vst::kPitchBend).
inline constexpr int kVst3CtrlPitchBend = 129;

/// Number of MIDI channels mapped on each input event bus.
inline constexpr int kVst3MidiChannels = 16;

/// Base of the reserved ParamID range used for MIDI controllers.
///
/// Chosen high enough that it cannot collide with author-assigned plug-in
/// parameter IDs (which are small integers) or the synthesized-bypass ID
/// (0x70427970). The reserved span is
///   [kVst3MidiCcParamBase, kVst3MidiCcParamBase + 16*130)
/// = [0xC0000000, 0xC0000820), i.e. 2080 IDs, all far above any realistic
/// plug-in parameter ID. The adapter additionally skips registering any
/// reserved ID that would collide with an existing store parameter, so the
/// guarantee holds even for a pathological plug-in that picks a high ID.
inline constexpr state::ParamID kVst3MidiCcParamBase = 0xC0000000u;

/// One past the last reserved controller ParamID.
inline constexpr state::ParamID kVst3MidiCcParamEnd =
    kVst3MidiCcParamBase +
    static_cast<state::ParamID>(kVst3MidiChannels * kVst3ControllersPerChannel);

/// Encode a (channel, controller) pair into its reserved ParamID.
/// `channel` is 0..15, `controller` is 0..129 (see header comment).
inline constexpr state::ParamID vst3_midi_cc_param_id(int channel,
                                                      int controller) noexcept {
    return kVst3MidiCcParamBase +
           static_cast<state::ParamID>(channel * kVst3ControllersPerChannel +
                                       controller);
}

/// True when `id` falls in the reserved MIDI-controller ParamID range.
inline constexpr bool is_vst3_midi_cc_param(state::ParamID id) noexcept {
    return id >= kVst3MidiCcParamBase && id < kVst3MidiCcParamEnd;
}

/// Decoded controller assignment.
struct Vst3MidiCcDecode {
    int channel = 0;     ///< 0..15
    int controller = 0;  ///< 0..129
};

/// Decode a reserved controller ParamID back into (channel, controller).
/// Precondition: is_vst3_midi_cc_param(id) is true.
inline constexpr Vst3MidiCcDecode vst3_decode_midi_cc_param(
    state::ParamID id) noexcept {
    const auto offset = static_cast<int>(id - kVst3MidiCcParamBase);
    return {offset / kVst3ControllersPerChannel,
            offset % kVst3ControllersPerChannel};
}

}  // namespace pulp::format::detail
