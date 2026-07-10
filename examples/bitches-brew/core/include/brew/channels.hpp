#pragma once

// Two channels, independently controlled.
//
// Every plug-in in the suite that processes or generates CV does so on a stereo
// bus, and the two channels are *not* a stereo pair — they are two unrelated
// control voltages that happen to share a plug-in. On an eight-output
// DC-coupled interface that is the difference between two CVs per instance and
// one CV plus a copy of it.
//
// So each per-channel control is registered twice, once per channel, and the
// audio callback reads the set belonging to the channel it is filling. The right
// channel's parameter IDs sit one fixed stride above the left's:
//
//     right_id = left_id + kRightChannelStride
//
// The stride is large enough that no plug-in will ever grow into it, and it is
// part of the persisted state contract, so it can never change. Deriving the
// right channel's ID rather than enumerating it means a control added to the
// left cannot be forgotten on the right — which is exactly the bug this scheme
// exists to make impossible.
//
// `Output Scale` and `Invert` are per-channel too, and that is not symmetry for
// its own sake: full-scale voltage and output polarity are properties of the
// physical jack, and an interface is free to differ between its own outputs.
//
// A host that hands us a mono bus simply never asks for channel 1, and the right
// channel's controls sit unused. Nothing needs to detect that.

#include <cstddef>

namespace pulp::examples::brew {

/// Channels a brew plug-in exposes per bus.
inline constexpr std::size_t kChannelCount = 2;

/// Distance in parameter-ID space between a control and its right-channel twin.
/// Persisted. Never change it.
inline constexpr int kRightChannelStride = 1000;

/// The parameter ID of `left_id`'s control on `channel`.
[[nodiscard]] inline constexpr int param_for(int left_id, std::size_t channel) noexcept {
    return channel == 0 ? left_id : left_id + kRightChannelStride;
}

/// Suffix for a per-channel parameter's display name. Hosts show a flat list, so
/// two controls called "Value" are two controls a user cannot tell apart.
[[nodiscard]] inline constexpr const char* channel_suffix(std::size_t channel) noexcept {
    return channel == 0 ? " L" : " R";
}

}  // namespace pulp::examples::brew
