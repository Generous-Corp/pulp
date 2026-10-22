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

/// Write `out[i] = fn(in[i])` for every sample, in index order.
///
/// This is the per-sample loop a processor writes for a stateless or
/// in-order stateful sample function (gain, mix, a waveshaper), written so it
/// vectorizes whether or not the host processes in place. A plain
/// `out[i] = f(in[i])` loop compiles to a runtime overlap check, and
/// `out == in` — the usual in-place call — fails that check and runs the
/// scalar fallback. Here the in-place case is its own single-pointer loop,
/// which the compiler can vectorize without a check.
///
/// `in` and `out` must be the same pointer or not overlap. `fn` is called
/// exactly once per sample, in increasing index order, so a function that
/// carries state from one sample to the next still sees samples in order.
template <typename Fn>
inline void map_samples(const float* in, float* out, std::size_t n, Fn&& fn) {
    if (in == out) {
        for (std::size_t i = 0; i < n; ++i) out[i] = fn(out[i]);
    } else {
        for (std::size_t i = 0; i < n; ++i) out[i] = fn(in[i]);
    }
}

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

/// Block level reading returned by `measure_peak_rms`: the peak absolute
/// sample value and the root-mean-square level. Both are 0.0f for an empty
/// input.
struct PeakRms {
    float peak = 0.0f;
    float rms = 0.0f;
};

/// Number of independent partial sums `measure_peak_rms` keeps. Sample `i`
/// is accumulated into partial `i % kPeakRmsLanes`; the partials are then
/// combined pairwise (lane k with lane k + 8, then k with k + 4, k + 2, and
/// finally 0 with 1). The order is fixed rather than tied to the host's SIMD
/// width, so the result is identical on every platform that agrees on FMA
/// contraction.
inline constexpr std::size_t kPeakRmsLanes = 16;

/// Measure the peak absolute value and RMS of a block in one pass.
///
/// This is the per-block meter reading a processor publishes for its UI. It
/// is allocation-free, safe on the audio thread, and vectorizes: independent
/// partial sums break the serial add chain a naive `sum += x * x` loop
/// forms. RMS is `sqrt(sum_of_squares / size)`.
///
/// Non-finite input: a NaN sample is ignored by `peak` (the same result as
/// folding with `std::max(peak, std::abs(x))`) but propagates to `rms`, so a
/// NaN in the block still shows up in the reading. An infinite sample makes
/// both values infinite.
PeakRms measure_peak_rms(std::span<const float> samples) noexcept;

/// Measure one channel of a multi-channel buffer.
PeakRms measure_peak_rms(const BufferView<float>& buffer, std::size_t channel) noexcept;
PeakRms measure_peak_rms(const BufferView<const float>& buffer, std::size_t channel) noexcept;

} // namespace pulp::audio::buffer_ops
