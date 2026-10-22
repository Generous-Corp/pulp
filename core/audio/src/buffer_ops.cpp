#include <pulp/audio/buffer_ops.hpp>

#include <pulp/runtime/simd.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

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
    // `std::clamp` and `simd_clamp` scalar tails propagate NaN through
    // ordered comparisons, so delegating directly would leak NaN into
    // downstream audio.
    // Sanitize NaN → `lo` ourselves first, then delegate the clamp.
    for (auto& s : samples) {
        if (std::isnan(s)) s = lo;
    }
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

namespace {

// Folds the 16 partials (peak[k], sum[k]) pairwise in the documented order and
// finishes the reading. Shared by both kernels so they agree bit for bit.
PeakRms finish_peak_rms(float* peak, float* sum, std::size_t n) noexcept {
    for (std::size_t width = kPeakRmsLanes / 2; width > 0; width /= 2) {
        for (std::size_t k = 0; k < width; ++k) {
            peak[k] = std::fmax(peak[k], peak[k + width]);
            sum[k] += sum[k + width];
        }
    }
    return {peak[0], std::sqrt(sum[0] / static_cast<float>(n))};
}

} // namespace

PeakRms measure_peak_rms(std::span<const float> samples) noexcept {
    const std::size_t n = samples.size();
    if (n == 0) return {};
    const float* x = samples.data();

    // Sixteen partials give four independent 4-wide accumulator chains, which
    // is what it takes to run at FP throughput rather than FMA latency on
    // current cores without making short blocks pay a long combine.
    constexpr std::size_t L = kPeakRmsLanes;
    static_assert(L == 16, "the vector kernel below holds four 4-lane groups");
    float peak[L] = {};
    float sum[L] = {};
    std::size_t i = 0;

#if defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_max) && __has_builtin(__builtin_elementwise_abs)
#define PULP_PEAK_RMS_VECTOR_KERNEL 1
#endif
#endif
#if defined(PULP_PEAK_RMS_VECTOR_KERNEL)
    // Same lanes and same per-lane operations as the portable loop below, held
    // in registers: the array form spills its accumulators to the stack.
    // elementwise_max is IEEE maxNum, which (like std::fmax) returns the
    // non-NaN operand.
    using V4 = float __attribute__((vector_size(16)));
    V4 p0{}, p1{}, p2{}, p3{}, s0{}, s1{}, s2{}, s3{};
    for (; i + L <= n; i += L) {
        V4 a, b, c, d;
        std::memcpy(&a, x + i, sizeof a);
        std::memcpy(&b, x + i + 4, sizeof b);
        std::memcpy(&c, x + i + 8, sizeof c);
        std::memcpy(&d, x + i + 12, sizeof d);
        p0 = __builtin_elementwise_max(p0, __builtin_elementwise_abs(a));
        p1 = __builtin_elementwise_max(p1, __builtin_elementwise_abs(b));
        p2 = __builtin_elementwise_max(p2, __builtin_elementwise_abs(c));
        p3 = __builtin_elementwise_max(p3, __builtin_elementwise_abs(d));
        s0 += a * a;
        s1 += b * b;
        s2 += c * c;
        s3 += d * d;
    }
    std::memcpy(peak, &p0, sizeof p0);
    std::memcpy(peak + 4, &p1, sizeof p1);
    std::memcpy(peak + 8, &p2, sizeof p2);
    std::memcpy(peak + 12, &p3, sizeof p3);
    std::memcpy(sum, &s0, sizeof s0);
    std::memcpy(sum + 4, &s1, sizeof s1);
    std::memcpy(sum + 8, &s2, sizeof s2);
    std::memcpy(sum + 12, &s3, sizeof s3);
#undef PULP_PEAK_RMS_VECTOR_KERNEL
#else
    // std::fmax returns the non-NaN operand, so a NaN sample never replaces
    // the running peak (peak itself starts at 0 and is never NaN).
    for (; i + L <= n; i += L) {
        for (std::size_t k = 0; k < L; ++k) {
            const float v = x[i + k];
            peak[k] = std::fmax(peak[k], std::fabs(v));
            sum[k] += v * v;
        }
    }
#endif
    for (std::size_t k = 0; i < n; ++i, ++k) {
        const float v = x[i];
        peak[k] = std::fmax(peak[k], std::fabs(v));
        sum[k] += v * v;
    }
    return finish_peak_rms(peak, sum, n);
}

PeakRms measure_peak_rms(const BufferView<float>& buffer, std::size_t channel) noexcept {
    return measure_peak_rms(std::span<const float>{buffer.channel(channel)});
}

PeakRms measure_peak_rms(const BufferView<const float>& buffer, std::size_t channel) noexcept {
    return measure_peak_rms(buffer.channel(channel));
}

} // namespace pulp::audio::buffer_ops
