#pragma once

// The parts of CV-to-OSC that have nothing to do with sockets.
//
// The whole plug-in turns on one rule: **the audio thread never sends a packet.**
// `sendto()` is a syscall. It can block, it can allocate inside the kernel, and
// it takes a lock somewhere in every network stack. Calling it from `process()`
// is an xrun waiting for a busy network. So the audio thread does exactly one
// thing — a relaxed atomic store of the latest sample — and a background thread
// reads that store on its own clock and sends.
//
// This costs nothing in fidelity, because OSC is not a sample-accurate transport
// in the first place. It is UDP: unordered, unacknowledged, and delivered when it
// gets there. Sending at 60 Hz what a 48 kHz signal is doing loses nothing a
// receiver could have used.
//
// Everything here is pure so the tests can pin the decisions without a socket.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace pulp::examples::brew {

/// Only in-computer, and deliberately. Sending control voltages to another
/// machine invites a firewall prompt on load and a stream of UDP at an address
/// the user never typed. If cross-machine ever lands it should be opt-in and
/// explicit, not a default nobody noticed.
inline constexpr const char* kOscHost = "127.0.0.1";

inline constexpr float kMinRateHz = 1.0f;
inline constexpr float kMaxRateHz = 500.0f;

/// `/brew/cv/0`, `/brew/cv/1`, ... One address per channel.
[[nodiscard]] inline std::string osc_address(std::size_t channel) {
    return "/brew/cv/" + std::to_string(channel);
}

/// Whether a value has moved far enough to be worth a packet.
///
/// A deadband, not a change test. Float noise on the last bit of an otherwise
/// steady voltage would otherwise flood the receiver at the full send rate, and
/// a receiver smoothing its input cannot tell that flood from a real signal.
///
/// `first` forces the very first send, so a receiver learns the resting value
/// rather than waiting for it to move. Without it, a DC source that never
/// changes would never announce itself, and the patch would look dead.
[[nodiscard]] inline bool should_send(bool first, float last_sent, float current,
                                      float deadband) noexcept {
    if (first) return true;
    if (!std::isfinite(current)) return false;
    return std::abs(current - last_sent) >= deadband;
}

/// Seconds between sends. Clamped, because a rate of zero is a divide by zero and
/// a rate of a million is a denial of service against the loopback interface.
[[nodiscard]] inline double send_interval_seconds(float rate_hz) noexcept {
    return 1.0 / static_cast<double>(std::clamp(rate_hz, kMinRateHz, kMaxRateHz));
}

}  // namespace pulp::examples::brew
