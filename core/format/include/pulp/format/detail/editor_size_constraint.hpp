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
    // The result is a box (w, round(w / ratio)): fixing the width and deriving
    // the height keeps a second pass over the result unchanged. The widest w
    // that fits the request is the smaller of the requested width and the
    // widest w whose derived height still fits the requested height.
    auto height_for = [ratio](std::uint32_t w) {
        return static_cast<std::uint32_t>(std::max<long>(1, std::lround(w / ratio)));
    };
    std::uint32_t w = width;
    if (height_for(w) > height) {
        w = static_cast<std::uint32_t>(
            std::max(1.0, std::floor((static_cast<double>(height) + 0.5) * ratio)));
        while (w > 1 && height_for(w) > height)
            --w;
    }
    // The widths whose derived height lies inside the height bounds, and
    // whose own value lies inside the width bounds.
    std::uint32_t lo = hints.min_width;
    {
        auto from_height = static_cast<std::uint32_t>(std::max(
            1.0, std::floor((static_cast<double>(hints.min_height) - 0.5) * ratio)));
        while (height_for(from_height) < hints.min_height)
            ++from_height;
        lo = std::max(lo, from_height);
    }
    std::uint32_t hi = hints.max_width > 0 ? hints.max_width : 0x7FFFFFFFu;
    if (hints.max_height > 0) {
        auto from_height = static_cast<std::uint32_t>(
            std::ceil((static_cast<double>(hints.max_height) + 0.5) * ratio));
        while (from_height > 1 && height_for(from_height) > hints.max_height)
            --from_height;
        hi = std::min(hi, from_height);
    }
    if (hi < lo)
        hi = lo; // contradictory bounds: the lower bound wins
    w = std::clamp(w, lo, hi);
    width = w;
    height = height_for(w);
    return true;
}

/// An initial size that fits inside a `max_width` x `max_height` area (a
/// screen's usable frame minus the window chrome). The size shrinks
/// proportionally, then goes through the hints so a locked aspect and the
/// minimums still hold; a minimum larger than the area wins over the area. A
/// size that already fits, a fixed-size editor, and an empty area leave the
/// size unchanged.
inline void fit_size_to_area(const ViewSize& hints, std::uint32_t& width, std::uint32_t& height,
                             std::uint32_t max_width, std::uint32_t max_height) {
    if (width == 0 || height == 0 || max_width == 0 || max_height == 0)
        return;
    if (width <= max_width && height <= max_height)
        return;
    if (!editor_resizable(hints))
        return;
    const double scale =
        std::min(static_cast<double>(max_width) / width, static_cast<double>(max_height) / height);
    auto w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::floor(width * scale)));
    auto h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::floor(height * scale)));
    constrain_to_hints(hints, w, h);
    width = w;
    height = h;
}

} // namespace pulp::format::detail
