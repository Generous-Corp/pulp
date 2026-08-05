#pragma once

#include "detail/audio_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace pulp::signal {

/// Orthonormal stereo transform. The 1/sqrt(2) normalization makes the
/// transform self-inverse and preserves L2 energy: L^2 + R^2 == M^2 + S^2.
/// IEEE non-finite samples propagate. Output buffers must not overlap each
/// other; either output may exactly alias either input.
template <typename SampleType>
inline void mid_side_encode(SampleType left, SampleType right, SampleType& mid,
                            SampleType& side) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    constexpr SampleType kInvSqrt2 =
        static_cast<SampleType>(0.707106781186547524400844362104849039L);
    mid = (left + right) * kInvSqrt2;
    side = (left - right) * kInvSqrt2;
}

template <typename SampleType>
inline void mid_side_decode(SampleType mid, SampleType side, SampleType& left,
                            SampleType& right) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    constexpr SampleType kInvSqrt2 =
        static_cast<SampleType>(0.707106781186547524400844362104849039L);
    left = (mid + side) * kInvSqrt2;
    right = (mid - side) * kInvSqrt2;
}

template <typename SampleType>
inline bool mid_side_encode_block(const SampleType* left, const SampleType* right, SampleType* mid,
                                  SampleType* side, std::size_t frames) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (left == nullptr || right == nullptr || mid == nullptr || side == nullptr ||
        detail::audio_ranges_overlap(mid, side, frames))
        return false;
    if ((mid != left && detail::audio_ranges_overlap(mid, left, frames)) ||
        (mid != right && detail::audio_ranges_overlap(mid, right, frames)) ||
        (side != left && detail::audio_ranges_overlap(side, left, frames)) ||
        (side != right && detail::audio_ranges_overlap(side, right, frames)))
        return false;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto l = left[i];
        const auto r = right[i];
        mid_side_encode(l, r, mid[i], side[i]);
    }
    return true;
}

template <typename SampleType>
inline bool mid_side_decode_block(const SampleType* mid, const SampleType* side, SampleType* left,
                                  SampleType* right, std::size_t frames) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (mid == nullptr || side == nullptr || left == nullptr || right == nullptr ||
        detail::audio_ranges_overlap(left, right, frames))
        return false;
    if ((left != mid && detail::audio_ranges_overlap(left, mid, frames)) ||
        (left != side && detail::audio_ranges_overlap(left, side, frames)) ||
        (right != mid && detail::audio_ranges_overlap(right, mid, frames)) ||
        (right != side && detail::audio_ranges_overlap(right, side, frames)))
        return false;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto m = mid[i];
        const auto s = side[i];
        mid_side_decode(m, s, left[i], right[i]);
    }
    return true;
}

/// Mono-safe width built on the orthonormal transform. Width is linear side
/// gain in [0, 2]; non-finite values select unity so malformed automation does
/// not collapse or explode the image. Returns the sanitized width.
template <typename SampleType>
inline SampleType stereo_width(SampleType left, SampleType right, SampleType width,
                               SampleType& out_left, SampleType& out_right) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (!std::isfinite(width))
        width = SampleType{1};
    width = std::clamp(width, SampleType{0}, SampleType{2});
    SampleType mid{}, side{};
    mid_side_encode(left, right, mid, side);
    mid_side_decode(mid, side * width, out_left, out_right);
    return width;
}

} // namespace pulp::signal
