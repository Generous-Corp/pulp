#pragma once

// The accept test for an AU v2 editor's own resize request. AU v2 has no
// resize request: the host follows the editor view's frame, so a request is
// accepted only when the frame ended at exactly the requested size and, for a
// host known to resize its enclosing container to match (Logic), the container
// did too. Plain doubles keep it free of AppKit and testable without a window.

#include <cmath>

namespace pulp::format::detail {

struct FrameSize {
    double width = 0.0;
    double height = 0.0;
};

constexpr double kFrameTolerance = 0.5;

inline bool same_frame_size(FrameSize a, FrameSize b) {
    return std::fabs(a.width - b.width) < kFrameTolerance &&
           std::fabs(a.height - b.height) < kFrameTolerance;
}

struct FramePoint {
    double x = 0.0;
    double y = 0.0;
};

inline bool same_frame_origin(FramePoint a, FramePoint b) {
    return std::fabs(a.x - b.x) < kFrameTolerance && std::fabs(a.y - b.y) < kFrameTolerance;
}

/// True when the request took. `container` is only consulted when
/// `host_resizes_container` and a container exists.
inline bool resize_request_accepted(FrameSize wanted, FrameSize view_after, bool editor_took_size,
                                    bool host_resizes_container, bool has_container,
                                    FrameSize container_after) {
    if (!editor_took_size || !same_frame_size(view_after, wanted))
        return false;
    if (host_resizes_container && has_container)
        return same_frame_size(container_after, wanted);
    return true;
}

} // namespace pulp::format::detail
