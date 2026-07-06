#include <pulp/view/native_view_host.hpp>

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>

namespace pulp::view {

namespace {

// A view that clips its descendants: a ScrollView viewport, or any View whose
// overflow clips its painted box. Must match View::paint_all, which clips on
// BOTH `hidden` and `scroll` (view.cpp) — a plain View (a `<div>`) can carry
// `Overflow::scroll` via the setOverflow bridge, design import, or codegen (it
// is NOT exclusive to ScrollView), and a plain View has no scroll offset to
// subtract (only ScrollView exposes scroll_x/scroll_y).
bool is_clip_container(const View* v) {
    if (dynamic_cast<const ScrollView*>(v)) return true;
    const View::Overflow o = v->overflow();
    return o == View::Overflow::hidden || o == View::Overflow::scroll;
}

} // namespace

NativeViewHost::~NativeViewHost() {
    if (attached_) detach_from_host();
}

void NativeViewHost::set_native_child(NativeViewHandle handle, SnapshotFn snapshot) {
    if (handle == handle_) {
        snapshot_ = std::move(snapshot);
        return;
    }
    // Detach the previous child (if any) before swapping handles — the host
    // tracks embedding by handle.
    if (attached_) detach_from_host();
    handle_ = handle;
    snapshot_ = std::move(snapshot);
    // A native overlay is invisible to the Skia headless-capture path; flag the
    // subtree so smart-capture composites the snapshot instead of returning
    // blank (see screenshot_gpu.cpp / capture_view).
    set_contains_native_overlay(handle_ != nullptr);
    request_repaint();  // paint_all re-attaches with correct geometry
}

void NativeViewHost::clear_native_child() {
    set_native_child(nullptr);
}

std::vector<uint8_t> NativeViewHost::capture_native_overlay_png(uint32_t width,
                                                                uint32_t height) {
    if (snapshot_) return snapshot_(width, height);
    return {};
}

void NativeViewHost::paint_all(canvas::Canvas& canvas) {
    // Paint the widget's own background / border (a placeholder that shows
    // through on platforms without native embedding, or before attach) and any
    // Skia children, then re-drive the native child's frame + clip. The native
    // view — when attached — composites ABOVE this paint.
    //
    // update_native_layout() touches AppKit/UIKit (NSView.frame, CALayer.mask)
    // via the host; that is safe because paint runs on the UI/main thread on
    // every host that implements the native-child primitive (Apple display-link
    // / drawRect). Revisit if paint is ever moved off the main thread.
    View::paint_all(canvas);
    update_native_layout();
}

void NativeViewHost::set_window_host(WindowHost* host) {
    View::set_window_host(host);  // propagate to children first
    reconcile_host();
}

void NativeViewHost::set_plugin_view_host(PluginViewHost* host) {
    View::set_plugin_view_host(host);  // propagate to children first
    reconcile_host();
}

void NativeViewHost::compute_geometry(Rect& frame, Rect& visible,
                                      bool& clipped) const {
    // Collect the ancestor chain self..root (self at index 0). Bounded: deep
    // trees beyond the cap treat the topmost collected node as the root, so at
    // pathological depths (>64) both the outer clips AND the accumulated
    // translation of ancestors above the cap are dropped — the frame would be
    // mispositioned. No real UI nests native embeds that deep.
    constexpr size_t kMaxDepth = 64;
    const View* chain[kMaxDepth];
    size_t n = 0;
    for (const View* v = this; v && n < kMaxDepth; v = v->parent())
        chain[n++] = v;

    // Walk top-down accumulating each view's absolute top-left origin. Mirrors
    // paint's transform: a child is positioned at parent_origin + child.bounds,
    // and a ScrollView shifts its children by -scroll (ComboBox::overlay_anchor_
    // uses the same peel). Intersect every clipping ancestor's viewport.
    float px = 0.0f, py = 0.0f;  // running parent absolute origin
    float cx0 = -1e30f, cy0 = -1e30f, cx1 = 1e30f, cy1 = 1e30f;  // clip accumulator
    float self_x = 0.0f, self_y = 0.0f;

    for (size_t i = n; i-- > 0;) {
        const View* v = chain[i];
        const Rect vb = v->bounds();
        float vx, vy;
        if (i + 1 < n) {
            vx = px + vb.x;
            vy = py + vb.y;
            if (auto* sv = dynamic_cast<const ScrollView*>(chain[i + 1])) {
                vx -= sv->scroll_x();
                vy -= sv->scroll_y();
            }
        } else {
            vx = vb.x;  // root
            vy = vb.y;
        }

        // Clip contribution of an ANCESTOR viewport (self never clips itself).
        // A ScrollView's clip is at its own origin (scroll applies to its
        // children, not its viewport rect).
        if (v != this && is_clip_container(v)) {
            cx0 = std::max(cx0, vx);
            cy0 = std::max(cy0, vy);
            cx1 = std::min(cx1, vx + vb.width);
            cy1 = std::min(cy1, vy + vb.height);
        }

        px = vx;
        py = vy;
        if (i == 0) {
            self_x = vx;
            self_y = vy;
        }
    }

    frame = {self_x, self_y, bounds().width, bounds().height};

    const float vx0 = std::max(frame.x, cx0);
    const float vy0 = std::max(frame.y, cy0);
    const float vx1 = std::min(frame.right(), cx1);
    const float vy1 = std::min(frame.bottom(), cy1);
    if (vx1 <= vx0 || vy1 <= vy0) {
        // Fully outside every clip — collapse to a zero-size visible rect at the
        // frame origin so the host masks the child away entirely.
        visible = {frame.x, frame.y, 0.0f, 0.0f};
        clipped = true;
    } else if (vx0 == frame.x && vy0 == frame.y && vx1 == frame.right() &&
               vy1 == frame.bottom()) {
        // No clip container trimmed any edge — emit the frame exactly (avoids a
        // float round-trip through width/height that could spuriously flip
        // `clipped` at large coordinates).
        visible = frame;
        clipped = false;
    } else {
        visible = {vx0, vy0, vx1 - vx0, vy1 - vy0};
        clipped = true;
    }
}

bool NativeViewHost::try_attach() {
    if (attached_ || !handle_) return false;
    Rect frame, visible;
    bool clipped = false;
    compute_geometry(frame, visible, clipped);

    // A view tree has at most one host kind; the plugin host takes precedence if
    // both were ever set (they normally are not).
    if (PluginViewHost* p = plugin_view_host()) {
        if (p->attach_native_child_view(handle_, frame.x, frame.y,
                                        frame.width, frame.height)) {
            attached_ = true;
            attached_kind_ = HostKind::plugin;
            attached_plugin_ = p;
        }
    } else if (WindowHost* w = window_host()) {
        if (w->attach_native_child_view(handle_, frame.x, frame.y,
                                        frame.width, frame.height)) {
            attached_ = true;
            attached_kind_ = HostKind::window;
            attached_window_ = w;
        }
    }

    if (attached_) {
        have_pushed_ = false;  // force the initial clip push
        push_geometry(frame, visible, clipped);
    }
    return attached_;
}

void NativeViewHost::detach_from_host() {
    if (!attached_) return;
    if (attached_kind_ == HostKind::plugin && attached_plugin_)
        attached_plugin_->detach_native_child_view(handle_);
    else if (attached_kind_ == HostKind::window && attached_window_)
        attached_window_->detach_native_child_view(handle_);
    attached_ = false;
    attached_kind_ = HostKind::none;
    attached_plugin_ = nullptr;
    attached_window_ = nullptr;
    have_pushed_ = false;
}

void NativeViewHost::reconcile_host() {
    if (!attached_) return;
    // Detach only when the host we attached THROUGH is no longer live. A
    // window-host change while attached to a plugin host (or vice-versa) is not
    // stale. Teardown nulls the back-reference while the host object is still
    // alive, so the stored raw pointer is safe to call here.
    bool stale = false;
    if (attached_kind_ == HostKind::plugin && plugin_view_host() != attached_plugin_)
        stale = true;
    if (attached_kind_ == HostKind::window && window_host() != attached_window_)
        stale = true;
    if (stale) detach_from_host();
}

void NativeViewHost::push_geometry(const Rect& frame, const Rect& visible,
                                   bool clipped) {
    if (!attached_) return;
    if (have_pushed_ && frame == pushed_frame_ && visible == pushed_visible_ &&
        clipped == pushed_clipped_)
        return;

    // The clip is the visible sub-rectangle expressed in the child's own
    // [0,0,width,height] box (top-left), decoupling the host mask math from any
    // container coordinate flip.
    const float local_x = visible.x - frame.x;
    const float local_y = visible.y - frame.y;

    if (attached_kind_ == HostKind::plugin && attached_plugin_) {
        attached_plugin_->set_native_child_view_bounds(handle_, frame.x, frame.y,
                                                       frame.width, frame.height);
        attached_plugin_->set_native_child_view_clip(handle_, clipped, local_x,
                                                     local_y, visible.width,
                                                     visible.height);
    } else if (attached_kind_ == HostKind::window && attached_window_) {
        attached_window_->set_native_child_view_bounds(handle_, frame.x, frame.y,
                                                       frame.width, frame.height);
        attached_window_->set_native_child_view_clip(handle_, clipped, local_x,
                                                     local_y, visible.width,
                                                     visible.height);
    }

    pushed_frame_ = frame;
    pushed_visible_ = visible;
    pushed_clipped_ = clipped;
    have_pushed_ = true;
}

void NativeViewHost::update_native_layout() {
    // A hidden widget hides its native child: the OS-composited view would
    // otherwise linger above the GPU layer even though the widget is invisible.
    // (An invisible ANCESTOR is not observable here — the parent's paint loop
    // stops before descending — so ancestor-driven hiding is out of scope, the
    // same limitation the GPU paint tree has for native overlays.)
    if (!visible()) {
        if (attached_) detach_from_host();
        return;
    }

    Rect frame, visible_rect;
    bool clipped = false;
    compute_geometry(frame, visible_rect, clipped);
    // Keep introspection getters current regardless of platform/attach state.
    last_frame_ = frame;
    last_visible_ = visible_rect;
    last_clipped_ = clipped;

    if (!attached_) {
        try_attach();  // recomputes + pushes internally on success
        return;
    }
    push_geometry(frame, visible_rect, clipped);
}

} // namespace pulp::view
