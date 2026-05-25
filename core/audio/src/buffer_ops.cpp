#include <pulp/audio/buffer_ops.hpp>

#include <pulp/runtime/simd.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio::buffer_ops {

void apply_gain(std::span<float> samples, float gain) {
    if (samples.empty()) return;
    // simd_scale handles in-place dst==a.
    pulp::runtime::simd_scale(samples.data(), gain, samples.data(), samples.size());
}

void apply_gain(BufferView<float> buffer, float gain) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        apply_gain(buffer.channel(ch), gain);
    }
}

void apply_gain_ramp(std::span<float> samples, float start_gain, float end_gain) {
    const std::size_t n = samples.size();
    if (n == 0) return;
    if (n == 1) {
        samples[0] *= start_gain;
        return;
    }
    const float step = (end_gain - start_gain) / static_cast<float>(n - 1);
    float g = start_gain;
    for (std::size_t i = 0; i < n; ++i) {
        samples[i] *= g;
        g += step;
    }
}

void apply_gain_ramp(BufferView<float> buffer, float start_gain, float end_gain) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        apply_gain_ramp(buffer.channel(ch), start_gain, end_gain);
    }
}

void clip(std::span<float> samples, float lo, float hi) {
    if (samples.empty()) return;
    // simd_clamp turns NaN into `lo` because std::min/std::max are
    // ordered comparisons that return the second arg on NaN inputs in
    // the Highway implementation Pulp uses. Scalar callers that hit
    // this path through the buffer overload get the same semantics
    // courtesy of `simd_clamp`'s scalar tail.
    pulp::runtime::simd_clamp(samples.data(), lo, hi, samples.data(), samples.size());
}

void clip(BufferView<float> buffer, float lo, float hi) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        clip(buffer.channel(ch), lo, hi);
    }
}

MinMax find_min_max(std::span<const float> samples) {
    if (samples.empty()) return {};
    return {
        pulp::runtime::simd_reduce_min(samples.data(), samples.size()),
        pulp::runtime::simd_reduce_max(samples.data(), samples.size()),
    };
}

MinMax find_min_max(const BufferView<float>& buffer, std::size_t channel) {
    return find_min_max(buffer.channel(channel));
}

MinMax find_min_max(const BufferView<const float>& buffer, std::size_t channel) {
    return find_min_max(buffer.channel(channel));
}

float find_magnitude(std::span<const float> samples) {
    if (samples.empty()) return 0.0f;
    // Peak magnitude = max(|min|, |max|). Cheaper than `simd_abs` into
    // a temporary buffer + a separate max reduce, which would also
    // require an allocation.
    const float lo = pulp::runtime::simd_reduce_min(samples.data(), samples.size());
    const float hi = pulp::runtime::simd_reduce_max(samples.data(), samples.size());
    return std::max(std::fabs(lo), std::fabs(hi));
}

float find_magnitude(const BufferView<float>& buffer, std::size_t channel) {
    return find_magnitude(buffer.channel(channel));
}

float find_magnitude(const BufferView<const float>& buffer, std::size_t channel) {
    return find_magnitude(buffer.channel(channel));
}

} // namespace pulp::audio::buffer_ops
