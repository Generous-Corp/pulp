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

#if defined(PULP_HAS_SDL3)
#include <pulp/view/sdl_window_host.hpp>
#endif

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>

#if defined(PULP_VIEW_HAS_RENDER_LOOP)
#include <pulp/render/render_loop.hpp>
#endif

#include <mutex>
#include <utility>

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
    damage_.mark_full();
    schedule_repaint();
}

// Bounded dirty mark. The union / sticky-full / degenerate-rect rules all live
// in PendingDamage::mark() — the same code the plug-in host runs — so the two
// hosts cannot disagree about what a bounded mark means.
void WindowHost::mark_dirty(const Rect& root_rect) {
    damage_.mark(root_rect);
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
    //
    // take() reads AND clears in one step, so this cannot paint one region and
    // then clear a different one — the frame is composed against exactly the
    // snapshot it consumed. This host paints straight onto a caller-supplied
    // canvas with no present stage that can fail, so the snapshot is never
    // restored.
    const PendingDamage::Snapshot frame = damage_.take();
    const bool bounded = surface_preserves_outside_clip && frame.is_bounded() &&
                         frame.bounds().width > 0.0f &&
                         frame.bounds().height > 0.0f;
    if (bounded) {
        canvas.save();
        canvas.clip_rect(frame.bounds().x, frame.bounds().y,
                         frame.bounds().width, frame.bounds().height);
    }
    root.paint_all(canvas);
    if (bounded) canvas.restore();
}

#if !defined(__APPLE__)

#if defined(PULP_HAS_SDL3)
// Standalone applications on Windows/Linux use the cross-platform SDL host.
// Keep the platform-neutral WindowHost contract at the boundary so callers
// (including standalone.cpp) do not need a second window API.  Host adapters
// may still install a richer native factory; this is the deterministic default
// for a standalone process that has no embedding host.
class SdlWindowHostAdapter final : public WindowHost {
  public:
    SdlWindowHostAdapter(View& root, const WindowOptions& options)
        : size_{static_cast<uint32_t>(options.width), static_cast<uint32_t>(options.height)} {
        SdlWindowHost::Options sdl_options;
        sdl_options.title = options.title;
        sdl_options.width = size_.width;
        sdl_options.height = size_.height;
        sdl_options.resizable = options.resizable;
        host_ = SdlWindowHost::create(root, std::move(sdl_options));
    }

    bool valid() const {
        return host_ != nullptr;
    }
    void show() override {
        if (host_) {
            host_->show();
            visible_ = true;
        }
    }
    void hide() override {
        if (host_) {
            host_->hide();
            visible_ = false;
        }
    }
    bool is_visible() const override {
        return visible_;
    }
    void repaint() override {
        if (host_)
            host_->repaint();
    }
    ContentSize get_content_size() const override {
        return size_;
    }
    void set_close_callback(std::function<void()> cb) override {
        if (host_)
            host_->set_close_callback(std::move(cb));
    }
    void run_event_loop() override {
        if (host_)
            host_->run_event_loop();
    }

  private:
    std::unique_ptr<SdlWindowHost> host_;
    ContentSize size_;
    bool visible_ = false;
};
#endif

std::unique_ptr<WindowHost> WindowHost::create(View& root,
                                                const WindowOptions& options) {
    // Copy the factory out, release the lock, then invoke. Creating a native
    // window can take milliseconds and the factory might call back into
    // set_factory/has_factory.
    WindowHost::Factory local;
    {
        std::lock_guard lock(g_factory_mu);
        if (g_factory_installed && g_factory)
            local = g_factory;
    }
    if (!local) {
#if defined(PULP_HAS_SDL3)
        auto fallback = std::make_unique<SdlWindowHostAdapter>(root, options);
        if (!fallback->valid())
            return nullptr;
        root.set_window_host(fallback.get());
        return fallback;
#else
        return nullptr;
#endif
    }
    auto host = local(root, options);
    if (host) {
        root.set_window_host(host.get());
    }
    return host;
}

#endif // !defined(__APPLE__)

} // namespace pulp::view
