// Non-Apple WindowHost fallback. Apple platforms have the
// real NSWindow/UIWindow impls in platform/mac / platform/ios and
// supply their own WindowHost::create(). On Windows/Linux/Android
// the host app registers a factory via set_factory(); without a
// factory, create() returns nullptr explicitly so callers can
// probe has_factory() to report "platform unsupported" instead of
// silently getting a null handle.
//
// The factory-registration API is compiled on every platform.

#include <pulp/view/window_host.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>

#if defined(PULP_VIEW_HAS_RENDER_LOOP)
#include <pulp/render/render_loop.hpp>
#endif

#include <mutex>

namespace pulp::view {

namespace {
    std::mutex            g_factory_mu;
    WindowHost::Factory   g_factory;
    bool                  g_factory_installed = false;
}

void WindowHost::set_factory(Factory factory) {
    std::lock_guard lock(g_factory_mu);
    g_factory = std::move(factory);
    g_factory_installed = true;
}

void WindowHost::clear_factory() {
    std::lock_guard lock(g_factory_mu);
    g_factory = {};
    g_factory_installed = false;
}

bool WindowHost::has_factory() {
    std::lock_guard lock(g_factory_mu);
    return g_factory_installed;
}

WindowHost::ContentSize WindowHost::get_content_size() const {
    return {};
}

void WindowHost::set_resize_callback(ResizeCallback cb) {
    (void) cb;
}

// VBlank-locked safe-repaint. When a RenderLoop is attached AND
// running, route the dirty mark through request_frame() so every call
// between two vblanks coalesces into a single vsync-paced repaint. Otherwise
// fall through to the platform repaint() — existing behavior, no regression.
//
// The is_running() guard matters: a loop attached but not yet started (or
// already stopped while still attached) has a no-op request_frame(), so
// without the guard the dirty update would be silently dropped and the UI
// could appear frozen until some other repaint trigger. Falling through to
// repaint() keeps an attached-but-idle loop correct.
//
// In GPU-off builds (no pulp-render target) RenderLoop is incomplete, so
// render_loop_ can never be non-null there and mark_dirty() is always a
// plain repaint(); the PULP_VIEW_HAS_RENDER_LOOP guard keeps it buildable.
void WindowHost::mark_dirty() {
    dirty_full_ = true;
    schedule_repaint();
}

// Bounded dirty mark. A dirty region only shrinks a full repaint on a frame
// that started clean (clear_pending_dirty() ran); dirty_full_ is sticky within
// a frame, so a bounded mark after a full mark (or on the first frame) leaves
// the repaint full — never widening coverage is what keeps this always safe.
void WindowHost::mark_dirty(const Rect& root_rect) {
    if (root_rect.width <= 0 || root_rect.height <= 0) {
        mark_dirty();  // empty region → be conservative, repaint in full
        return;
    }
    if (!dirty_full_) {
        if (!have_dirty_bounds_) {
            dirty_bounds_ = root_rect;
            have_dirty_bounds_ = true;
        } else {
            // Union into the running bounding box.
            const float nx = std::min(dirty_bounds_.x, root_rect.x);
            const float ny = std::min(dirty_bounds_.y, root_rect.y);
            dirty_bounds_ = {nx, ny,
                             std::max(dirty_bounds_.right(), root_rect.right()) - nx,
                             std::max(dirty_bounds_.bottom(), root_rect.bottom()) - ny};
        }
    }
    schedule_repaint();
}

void WindowHost::schedule_repaint() {
#if defined(PULP_VIEW_HAS_RENDER_LOOP)
    if (render_loop_ && render_loop_->is_running()) {
        render_loop_->request_frame();
        return;
    }
#endif
    repaint();
}

void WindowHost::paint_root(canvas::Canvas& canvas, View& root,
                            bool surface_preserves_outside_clip) {
    // Only clip when the caller affirms its surface preserves content outside the
    // clip AND a bounded (non-full) region is pending. Otherwise paint the whole
    // tree — the safe default that can never blank static chrome. (width/height
    // are already positive here: mark_dirty() escalates empty/zero rects to full,
    // so the size checks are belt-and-suspenders.)
    const bool bounded = surface_preserves_outside_clip &&
                         !dirty_full_ && have_dirty_bounds_ &&
                         dirty_bounds_.width > 0.0f && dirty_bounds_.height > 0.0f;
    if (bounded) {
        canvas.save();
        canvas.clip_rect(dirty_bounds_.x, dirty_bounds_.y,
                         dirty_bounds_.width, dirty_bounds_.height);
    }
    root.paint_all(canvas);
    if (bounded) canvas.restore();
    // The frame is composed; the next frame starts clean (a fresh full mark
    // until bounded marks arrive).
    clear_pending_dirty();
}

#if !defined(__APPLE__)

std::unique_ptr<WindowHost> WindowHost::create(View& root,
                                                const WindowOptions& options) {
    // Copy the factory out, release the lock, then invoke. Creating a native
    // window can take milliseconds and the factory might call back into
    // set_factory/has_factory.
    WindowHost::Factory local;
    {
        std::lock_guard lock(g_factory_mu);
        if (!g_factory_installed || !g_factory) return nullptr;
        local = g_factory;
    }
    auto host = local(root, options);
    if (host) {
        root.set_window_host(host.get());
    }
    return host;
}

#endif // !defined(__APPLE__)

} // namespace pulp::view
