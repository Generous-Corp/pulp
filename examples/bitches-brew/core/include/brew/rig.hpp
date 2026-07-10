#pragma once

// Rig analysis — the arithmetic behind the loopback doctor.
//
// Separated from the device driving so it can be tested without an audio
// interface. Everything here is a pure function of recorded samples; the CLI in
// rig/brew_rig.cpp supplies the samples and does nothing clever with them.
//
// The measurement this supports: drive DC on exactly one output channel, record
// every input channel, and see which one moved. Repeat per output channel and
// the result is the crossbar — which host channel index arrives at which jack.
// Nothing about that mapping is knowable from a datasheet; interfaces number
// their ADAT banks however they like.
//
// What a loopback CANNOT tell you: volts. A closed loop is dimensionless. Unity
// gain in means unity gain out, whether the wire carried 1 V or 10 V. Absolute
// scale needs an instrument, and until it is measured nothing in this suite may
// print a voltage.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::examples::brew {

/// What one input channel did while a test signal was playing.
struct ChannelStats {
    double mean = 0.0;   ///< DC component. The measurement, for a DC probe.
    float peak = 0.0f;   ///< Largest magnitude seen. Distinguishes dead from noisy.
};

[[nodiscard]] inline ChannelStats channel_stats(const float* samples,
                                                std::size_t count) noexcept {
    ChannelStats s;
    if (samples == nullptr || count == 0) return s;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const float v = samples[i];
        sum += static_cast<double>(v);
        const float a = std::abs(v);
        if (a > s.peak) s.peak = a;
    }
    s.mean = sum / static_cast<double>(count);
    return s;
}

/// One input channel's answer to one driven output channel.
struct Response {
    int input_channel = -1;
    /// Signed: `mean / drive_level`. Near +1 is a straight wire. Near -1 means
    /// the chain inverts polarity somewhere — which is exactly why every plug-in
    /// in this suite carries an `Invert` control.
    double gain = 0.0;
};

/// Which input channels responded to a drive of `level` on one output channel.
///
/// `threshold` is in gain units, not sample units, so it means the same thing at
/// every drive level. A channel is "responding" when |mean| exceeds
/// `threshold * |level|`. Ordered by input channel index.
[[nodiscard]] inline std::vector<Response> responding_channels(
    const std::vector<ChannelStats>& per_channel,
    double level,
    double threshold = 0.1) {
    std::vector<Response> out;
    if (!(std::abs(level) > 0.0)) return out;
    const double floor = threshold * std::abs(level);
    for (std::size_t c = 0; c < per_channel.size(); ++c) {
        const double mean = per_channel[c].mean;
        if (std::abs(mean) > floor)
            out.push_back({static_cast<int>(c), mean / level});
    }
    return out;
}

/// True when every input channel is *exactly* zero across the whole capture.
///
/// This is the signature of a permissions denial, not of an unpatched cable. A
/// real converter's input has a noise floor; a TCC-blocked capture on macOS
/// hands back frames of literal 0.0f with no error. Reporting the two as the
/// same thing costs an hour of chasing cables.
[[nodiscard]] inline bool all_exactly_zero(
    const std::vector<ChannelStats>& per_channel) noexcept {
    if (per_channel.empty()) return false;
    for (const auto& s : per_channel)
        if (s.peak != 0.0f) return false;
    return true;
}

/// Index of the first sample whose magnitude reaches `threshold`.
///
/// Used for round-trip latency: emit an impulse at a known sample, find when it
/// comes back. That number is what `Sync`'s `Offset` knob exists to cancel — the
/// DAC, its reconstruction filter, the ADC, and every buffer in between.
[[nodiscard]] inline std::optional<std::int64_t> first_crossing(
    const float* samples, std::size_t count, float threshold) noexcept {
    if (samples == nullptr) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i)
        if (std::abs(samples[i]) >= threshold)
            return static_cast<std::int64_t>(i);
    return std::nullopt;
}

/// Clamp a requested drive level into something that cannot surprise a modular.
///
/// Deliberately asymmetric with the rest of the suite: `resolve_output` clamps to
/// full scale because a plug-in's job is to emit what the user asked for. This
/// tool's job is to probe an unknown chain, so its ceiling is lower and its
/// default is lower still.
inline constexpr float kMaxProbeLevel = 0.5f;

[[nodiscard]] inline constexpr float clamp_probe_level(float level) noexcept {
    if (level > kMaxProbeLevel) return kMaxProbeLevel;
    if (level < -kMaxProbeLevel) return -kMaxProbeLevel;
    return level;
}

}  // namespace pulp::examples::brew
