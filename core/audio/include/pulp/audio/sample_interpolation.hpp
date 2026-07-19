#pragma once

#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/sample_sinc_kernel.hpp>
#include <pulp/signal/interpolator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace pulp::audio {

inline constexpr std::uint32_t kMaximumSampleInterpolationTaps = 128;

enum class SampleInterpolationPolicy : std::uint8_t {
    Hold = 0,
    Nearest = 1,
    Linear = 2,
    CubicHermite = 3,
    CubicLagrange = 4,
    RatioTrackingSinc = 5,
};

/// Stable serialization/UI metadata for one interpolation policy. `id` is the
/// machine-facing spelling and must not be changed once published; `name` is
/// the concise user-facing label. The explicit enum ordinals above preserve
/// the existing PulpSampler parameter contract (0 through 5).
struct SampleInterpolationPolicyInfo {
    SampleInterpolationPolicy policy = SampleInterpolationPolicy::Hold;
    std::string_view id;
    std::string_view name;
};

inline constexpr std::array<SampleInterpolationPolicyInfo, 6>
    kSampleInterpolationPolicies{{
        {SampleInterpolationPolicy::Hold, "hold", "Hold"},
        {SampleInterpolationPolicy::Nearest, "nearest", "Nearest"},
        {SampleInterpolationPolicy::Linear, "linear", "Linear"},
        {SampleInterpolationPolicy::CubicHermite,
         "cubic-hermite", "Cubic Hermite"},
        {SampleInterpolationPolicy::CubicLagrange,
         "cubic-lagrange", "Cubic Lagrange"},
        {SampleInterpolationPolicy::RatioTrackingSinc,
         "ratio-tracking-sinc", "Ratio-tracking sinc"},
    }};

constexpr const SampleInterpolationPolicyInfo* sample_interpolation_policy_info(
    SampleInterpolationPolicy policy) noexcept {
    for (const auto& candidate : kSampleInterpolationPolicies)
        if (candidate.policy == policy) return &candidate;
    return nullptr;
}

constexpr const SampleInterpolationPolicyInfo* sample_interpolation_policy_info(
    std::string_view id) noexcept {
    for (const auto& candidate : kSampleInterpolationPolicies)
        if (candidate.id == id) return &candidate;
    return nullptr;
}

constexpr std::string_view sample_interpolation_policy_id(
    SampleInterpolationPolicy policy) noexcept {
    const auto* info = sample_interpolation_policy_info(policy);
    return info == nullptr ? std::string_view{} : info->id;
}

constexpr std::string_view sample_interpolation_policy_name(
    SampleInterpolationPolicy policy) noexcept {
    const auto* info = sample_interpolation_policy_info(policy);
    return info == nullptr ? std::string_view{"Unknown"} : info->name;
}

struct SampleInterpolationFootprint {
    std::int32_t first_offset = 0;
    std::uint32_t tap_count = 0;
};

constexpr bool valid_sample_interpolation(
    SampleInterpolationPolicy policy) noexcept {
    switch (policy) {
        case SampleInterpolationPolicy::Hold:
        case SampleInterpolationPolicy::Nearest:
        case SampleInterpolationPolicy::Linear:
        case SampleInterpolationPolicy::CubicHermite:
        case SampleInterpolationPolicy::CubicLagrange:
        case SampleInterpolationPolicy::RatioTrackingSinc:
            return true;
    }
    return false;
}

constexpr SampleInterpolationFootprint sample_interpolation_footprint(
    SampleInterpolationPolicy policy,
    float fraction,
    const SampleSincKernelSelection& sinc = {}) noexcept {
    switch (policy) {
        case SampleInterpolationPolicy::Hold:
            return {0, 1};
        case SampleInterpolationPolicy::Nearest:
            return {fraction < 0.5f ? 0 : 1, 1};
        case SampleInterpolationPolicy::Linear:
            return {0, 2};
        case SampleInterpolationPolicy::CubicHermite:
        case SampleInterpolationPolicy::CubicLagrange:
            return {-1, 4};
        case SampleInterpolationPolicy::RatioTrackingSinc:
            return sinc.valid()
                ? SampleInterpolationFootprint{
                      sinc.wider.first_offset(), sinc.wider.tap_count()}
                : SampleInterpolationFootprint{};
    }
    return {};
}

inline float evaluate_sample_interpolation(
    SampleInterpolationPolicy policy,
    float fraction,
    std::span<const float> taps,
    const SampleSincKernelSelection& sinc = {}) noexcept {
    const auto footprint = sample_interpolation_footprint(policy, fraction, sinc);
    if (footprint.tap_count == 0 || taps.size() != footprint.tap_count)
        return 0.0f;

    switch (policy) {
        case SampleInterpolationPolicy::Hold:
        case SampleInterpolationPolicy::Nearest:
            return taps[0];
        case SampleInterpolationPolicy::Linear:
            return signal::Interpolator::linear(fraction, taps[0], taps[1]);
        case SampleInterpolationPolicy::CubicHermite:
            return signal::Interpolator::hermite(
                fraction, taps[0], taps[1], taps[2], taps[3]);
        case SampleInterpolationPolicy::CubicLagrange:
            return signal::Interpolator::lagrange(
                fraction, taps[0], taps[1], taps[2], taps[3]);
        case SampleInterpolationPolicy::RatioTrackingSinc:
            return sinc.apply(taps, fraction);
    }
    return 0.0f;
}

struct PreparedSampleInterpolation {
    SampleInterpolationPolicy policy = SampleInterpolationPolicy::Hold;
    SampleSincKernelSelection sinc{};

    bool valid() const noexcept {
        if (!valid_sample_interpolation(policy) ||
            (policy == SampleInterpolationPolicy::RatioTrackingSinc &&
             !sinc.valid())) {
            return false;
        }
        const auto span = footprint(0.0f);
        return span.tap_count > 0 &&
               span.tap_count <= kMaximumSampleInterpolationTaps;
    }

    SampleInterpolationFootprint footprint(float fraction) const noexcept {
        return sample_interpolation_footprint(policy, fraction, sinc);
    }

    std::uint32_t guard_frames() const noexcept {
        std::uint32_t guard = 0;
        for (const auto fraction : {0.0f, 0.5f}) {
            const auto span = footprint(fraction);
            const auto last_offset = span.tap_count == 0
                ? span.first_offset
                : span.first_offset +
                      static_cast<std::int32_t>(span.tap_count - 1);
            guard = std::max(guard, static_cast<std::uint32_t>(std::max(
                std::abs(span.first_offset), std::abs(last_offset))));
        }
        return guard;
    }

    float evaluate(float fraction, std::span<const float> taps) const noexcept {
        return evaluate_sample_interpolation(policy, fraction, taps, sinc);
    }
};

static_assert(std::is_trivially_copyable_v<PreparedSampleInterpolation>);

inline bool same_sample_interpolation(
    const PreparedSampleInterpolation& left,
    const PreparedSampleInterpolation& right) noexcept {
    const auto same_kernel = [](const SampleSincKernelView& a,
                                const SampleSincKernelView& b) noexcept {
        return a.storage_identity() == b.storage_identity() &&
               a.half_width() == b.half_width() &&
               a.phases() == b.phases() && a.cutoff() == b.cutoff();
    };
    return left.policy == right.policy &&
           same_kernel(left.sinc.wider, right.sinc.wider) &&
           same_kernel(left.sinc.narrower, right.sinc.narrower) &&
           left.sinc.narrower_gain == right.sinc.narrower_gain;
}

struct ResolvedSampleInterpolationPosition {
    std::int64_t base_frame = 0;
    float fraction = 0.0f;
    SampleInterpolationFootprint footprint{};
    bool valid = false;
};

inline ResolvedSampleInterpolationPosition resolve_sample_interpolation_position(
    double normalized_position,
    const PreparedSampleInterpolation& interpolation) noexcept {
    ResolvedSampleInterpolationPosition result;
    const auto minimum_safe_position = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::min()), 0.0);
    const auto maximum_safe_position = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::max()), 0.0);
    if (!interpolation.valid() || !std::isfinite(normalized_position) ||
        normalized_position <= minimum_safe_position ||
        normalized_position >= maximum_safe_position) {
        return result;
    }
    result.base_frame = static_cast<std::int64_t>(
        std::floor(normalized_position));
    result.fraction = static_cast<float>(
        normalized_position - static_cast<double>(result.base_frame));
    result.footprint = interpolation.footprint(result.fraction);
    result.valid = result.footprint.tap_count > 0 &&
                   result.footprint.tap_count <=
                       kMaximumSampleInterpolationTaps;
    return result;
}

constexpr bool valid_loop_interpolation(LoopInterpolationMode mode) noexcept {
    switch (mode) {
        case LoopInterpolationMode::None:
        case LoopInterpolationMode::Linear:
        case LoopInterpolationMode::Cubic:
            return true;
    }
    return false;
}

constexpr SampleInterpolationPolicy sample_interpolation_policy(
    LoopInterpolationMode mode) noexcept {
    switch (mode) {
        case LoopInterpolationMode::None:
            return SampleInterpolationPolicy::Hold;
        case LoopInterpolationMode::Linear:
            return SampleInterpolationPolicy::Linear;
        case LoopInterpolationMode::Cubic:
            return SampleInterpolationPolicy::CubicHermite;
    }
    return SampleInterpolationPolicy::Hold;
}

}  // namespace pulp::audio
