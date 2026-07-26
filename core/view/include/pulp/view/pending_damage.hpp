#pragma once

#include <pulp/view/geometry.hpp>

#include <algorithm>

namespace pulp::view {

/// Accumulated repaint damage for one frame, in root/window coordinates.
///
/// Extracted so the WINDOW host and the PLUG-IN host can share one definition
/// of "what changed this frame". Before this existed the state lived only on
/// WindowHost, so `View::request_repaint(Rect)` had nowhere to put a bounded
/// rect on the plug-in path and discarded it — which meant partial repaint was
/// unreachable for every plug-in editor on every platform, however the host was
/// wired.
///
/// The invariants are what make bounded damage safe to ignore or honour:
///
///  - `full` is STICKY within a frame. A bounded mark after a full one leaves
///    the repaint full. Damage therefore only ever SHRINKS a repaint, never
///    widens one, so a host that never consults `bounds()` is always correct.
///  - An empty/degenerate rect escalates to full rather than being dropped.
///  - `full` starts true: the first frame of any surface is a full repaint.
class PendingDamage {
public:
    /// Whole-surface repaint. Any structural change uses this.
    void mark_full() { full_ = true; }

    /// Union a bounded region (root coords) into this frame's damage.
    void mark(const Rect& root_rect) {
        if (root_rect.width <= 0 || root_rect.height <= 0) {
            mark_full();  // degenerate region → be conservative
            return;
        }
        if (full_) return;  // sticky: never widen back out of a full repaint
        if (!have_bounds_) {
            bounds_ = root_rect;
            have_bounds_ = true;
            return;
        }
        const float nx = std::min(bounds_.x, root_rect.x);
        const float ny = std::min(bounds_.y, root_rect.y);
        bounds_ = {nx, ny,
                   std::max(bounds_.right(), root_rect.right()) - nx,
                   std::max(bounds_.bottom(), root_rect.bottom()) - ny};
    }

    bool is_full() const { return full_; }
    bool has_bounds() const { return have_bounds_; }
    Rect bounds() const { return bounds_; }

    /// Called by the host once it has painted and submitted the frame.
    void clear() {
        full_ = false;
        have_bounds_ = false;
        bounds_ = {};
    }

private:
    bool full_ = true;  ///< first frame is always full
    bool have_bounds_ = false;
    Rect bounds_{};
};

}  // namespace pulp::view
