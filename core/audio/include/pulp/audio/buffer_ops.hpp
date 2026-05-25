#pragma once

/// @file buffer_ops.hpp
/// Vectorised buffer operations on `pulp::audio::BufferView<float>`.
///
/// These helpers wrap the SIMD primitives in `pulp::runtime::simd`
/// with audio-friendly semantics: in-place gain, gain ramps, hard
/// clipping, and min/max + magnitude scans. They are allocation-free
/// and safe to call from the audio thread.
///
/// Layout choice:
///   - the channel-iterating overloads walk every channel of a
///     `BufferView` and call the per-span path.
///   - the per-span path dispatches directly to `simd_*`, so it is
///     the right entry point for inner loops that already hold a
///     `std::span<float>` (e.g. inside a single-channel processor).
///
/// Ramp helpers (`apply_gain_ramp`) keep the increment scalar so
/// successive samples step in exactly one ULP intervals — vectorising
/// the ramp loop would either introduce off-by-N rounding or require
/// a per-lane initial offset table that costs more than the scalar
/// walk on modern OoO cores.

#include <pulp/audio/buffer.hpp>

#include <cstddef>
#include <span>

namespace pulp::audio::buffer_ops {

/// Apply a constant gain to a single channel of samples in place.
void apply_gain(std::span<float> samples, float gain);

/// Apply a constant gain to every channel of a multi-channel buffer.
void apply_gain(BufferView<float> buffer, float gain);

/// Apply a linear gain ramp from `start_gain` to `end_gain` across the
/// span. The ramp is sample-accurate: `samples[0]` is multiplied by
/// `start_gain`, `samples[N-1]` by `end_gain`, with `(end - start) /
/// (N - 1)` step between adjacent samples (or `start` for a 1-sample
/// span). A 0-length span is a no-op.
void apply_gain_ramp(std::span<float> samples, float start_gain, float end_gain);

/// Apply the same gain ramp to every channel of a multi-channel buffer.
void apply_gain_ramp(BufferView<float> buffer, float start_gain, float end_gain);

/// Hard-clip every sample to `[lo, hi]` in place. NaN inputs become
/// `lo` (matches the audio convention of treating non-finite samples
/// as silence rather than letting them propagate).
void clip(std::span<float> samples, float lo, float hi);

/// Hard-clip every channel of a multi-channel buffer to `[lo, hi]`.
void clip(BufferView<float> buffer, float lo, float hi);

/// Pair returned by `find_min_max`. `min` and `max` are both 0.0f for
/// an empty input (a defensible default for callers that build meters
/// or peak displays from this output).
struct MinMax {
    float min = 0.0f;
    float max = 0.0f;
};

/// Find the min and max sample values in a span. Empty span yields
/// `{0, 0}`.
MinMax find_min_max(std::span<const float> samples);

/// Find the min and max for one channel of a multi-channel buffer.
MinMax find_min_max(const BufferView<float>& buffer, std::size_t channel);
MinMax find_min_max(const BufferView<const float>& buffer, std::size_t channel);

/// Find the peak absolute value (magnitude) in a span. Empty span
/// yields `0.0f`.
float find_magnitude(std::span<const float> samples);

/// Find the peak absolute value across one channel of a multi-channel
/// buffer.
float find_magnitude(const BufferView<float>& buffer, std::size_t channel);
float find_magnitude(const BufferView<const float>& buffer, std::size_t channel);

} // namespace pulp::audio::buffer_ops
