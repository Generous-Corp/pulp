#pragma once

// Pure size arithmetic for an editor's declared ViewSize hints. The contract
// adapters (VST3 checkSizeConstraint, CLAP adjust_size, the AU v2 container)
// share it through ContractEditorSession::constrain(); a test or a widget can
// call it without opening an editor.

#include <pulp/format/plugin_descriptor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::format::detail {

/// True when the host may resize an editor with these hints: both minimums
/// are set (the convention the ViewBridge adapters use, so a default
/// ViewSize is fixed-size), and the bounds leave some room.
constexpr bool editor_resizable(const ViewSize& hints) {
    if (hints.min_width == 0 || hints.min_height == 0)
        return false;
    return !(hints.min_width == hints.max_width && hints.min_height == hints.max_height);
}

/// Bring (width, height) inside a resizable editor's hints. With an aspect
/// ratio the result is the largest box at that ratio that fits the request,
/// then moved inside min/max without leaving the ratio; without one each axis
/// is clamped on its own. False for a zero request or a non-resizable editor,
/// leaving the arguments untouched.
inline bool constrain_to_hints(const ViewSize& hints, std::uint32_t& width, std::uint32_t& height) {
    if (width == 0 || height == 0 || !editor_resizable(hints))
        return false;
    auto clamp = [](std::uint32_t v, std::uint32_t lo, std::uint32_t hi) {
        if (hi > 0 && v > hi)
            v = hi;
        return std::max(v, lo);
    };
    if (!(hints.aspect_ratio > 0.0)) {
        width = clamp(width, hints.min_width, hints.max_width);
        height = clamp(height, hints.min_height, hints.max_height);
        return true;
    }
    const double ratio = hints.aspect_ratio;
    const double requested = static_cast<double>(width) / static_cast<double>(height);
    double w = width;
    if (requested > ratio)
        w = static_cast<double>(height) * ratio;
    // The widths a box at `ratio` can have inside both axes' bounds.
    double lo =
        std::max<double>(hints.min_width, std::ceil(static_cast<double>(hints.min_height) * ratio));
    double hi = 1e9;
    if (hints.max_width > 0)
        hi = std::min<double>(hi, hints.max_width);
    if (hints.max_height > 0)
        hi = std::min<double>(hi, std::floor(static_cast<double>(hints.max_height) * ratio));
    if (lo > hi) {
        // Bounds that cannot hold the ratio: the width limits win.
        lo = hints.min_width;
        hi = hints.max_width > 0 ? hints.max_width : 1e9;
    }
    hi = std::max(lo, hi);
    w = std::clamp(std::round(w), lo, hi);
    width = static_cast<std::uint32_t>(w);
    height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(w / ratio)));
    return true;
}

} // namespace pulp::format::detail
