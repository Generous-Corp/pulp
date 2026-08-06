// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace pulp::import_design::browser_capture {

inline constexpr int kDefaultDeviceScaleFactor = 2;
inline constexpr int kMaximumLogicalViewportDimension = 8192;
inline constexpr std::uint64_t kMaximumCaptureDevicePixels =
    64ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool viewport_within_capture_limits(
    int width, int height, int device_scale_factor) noexcept {
    if (width <= 0 || height <= 0 || device_scale_factor <= 0 ||
        width > kMaximumLogicalViewportDimension ||
        height > kMaximumLogicalViewportDimension) {
        return false;
    }
    const auto device_pixels =
        static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) *
        static_cast<std::uint64_t>(device_scale_factor) *
        static_cast<std::uint64_t>(device_scale_factor);
    return device_pixels <= kMaximumCaptureDevicePixels;
}

}  // namespace pulp::import_design::browser_capture
