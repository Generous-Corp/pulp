// Emscripten WindowHost: paints a Pulp View tree into an HTMLCanvasElement
// through the Ganesh/WebGL2 SkiaSurface, paced by a requestAnimationFrame
// RenderLoop.
//
// Compiled only for Emscripten targets (listed by tools/cmake/PulpWebUi.cmake,
// deliberately NOT by core/view/CMakeLists.txt — CMake defines UNIX for
// Emscripten, so the Linux branch there would otherwise pull in AT-SPI and
// WebKitGTK sources).

#include <pulp/view/window_host.hpp>

#if defined(__EMSCRIPTEN__)

#include <pulp/canvas/canvas.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/render/headless_surface.hpp>
#include <pulp/render/render_loop.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/continuous_frames.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/web/web_event_translate.hpp>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::view::web {
namespace {

class BrowserWindowHost;

// The page owns one canvas-backed host at a time; the DOM resize listener has
// no other handle to reach it.
BrowserWindowHost* g_live_host = nullptr;

class BrowserWindowHost final : public WindowHost {
public:
    BrowserWindowHost(View& root, WindowOptions options, std::string canvas_selector)
        : root_(root),
          options_(std::move(options)),
          canvas_selector_(std::move(canvas_selector)),
          router_(root) {
        css_width_ = options_.width;
        css_height_ = options_.height;
        float measured_w = 0, measured_h = 0;
        if (canvas_css_size(canvas_selector_, measured_w, measured_h) &&
            measured_w > 0 && measured_h > 0) {
            css_width_ = measured_w;
            css_height_ = measured_h;
        }

        create_surface();

        root_.set_frame_clock(&frame_clock_);
        router_.set_dirty_callback([this] { mark_dirty(); });
        sync_router_mapping();
        install_web_input_listeners(canvas_selector_, router_);
    }

    ~BrowserWindowHost() override {
        alive_->store(false, std::memory_order_release);
        if (g_live_host == this) g_live_host = nullptr;
        remove_web_input_listeners(canvas_selector_);
        if (render_loop_owned_) render_loop_owned_->stop();
        attach_render_loop(nullptr);
        shutdown_dispatcher();
        root_.set_frame_clock(nullptr);
    }

    // ── WindowHost pure virtuals ────────────────────────────────────────

    void show() override {
        visible_ = true;
        set_canvas_visible(canvas_selector_, true);
        mark_dirty();
    }

    void hide() override {
        visible_ = false;
        set_canvas_visible(canvas_selector_, false);
    }

    bool is_visible() const override { return visible_; }

    void repaint() override {
        // Fallback path only: mark_dirty() routes through the RenderLoop once
        // run_event_loop() has armed it, so a direct repaint here would double
        // up with the next rAF callback.
        render_frame();
    }

    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
    }

    /// NON-BLOCKING BY CONTRACT. A browser has exactly one event loop and it is
    /// owned by the page: blocking here (a spin, or emscripten_set_main_loop's
    /// unwind) would deadlock the very loop that delivers our rAF callbacks and
    /// DOM events. So this arms the rAF RenderLoop and RETURNS IMMEDIATELY.
    ///
    /// Callers that do teardown after run_event_loop() returns must move it into
    /// the close callback (set_close_callback) — on the web the host outlives
    /// this call and is torn down by request_close() or page unload.
    void run_event_loop() override {
        if (running_) return;
        running_ = true;

        show();
        register_dispatcher();

        render_loop_owned_ = render::RenderLoop::create();
        if (render_loop_owned_) {
            attach_render_loop(render_loop_owned_.get());
            render_loop_owned_->start([this] { on_frame(); });
            render_loop_owned_->request_frame();
        } else {
            // No frame source: paint once so the canvas is not blank, and let
            // mark_dirty() fall through to repaint() for later updates.
            render_frame();
        }
    }

    // ── Geometry / HiDPI ────────────────────────────────────────────────

    ContentSize get_content_size() const override {
        return {static_cast<uint32_t>(std::max(0.0f, css_width_)),
                static_cast<uint32_t>(std::max(0.0f, css_height_))};
    }

    float dpi_scale() const override { return dpr_; }

    // False while the WebGL context is lost (the Ganesh surface abandons its
    // Skia context and refuses to hand out a canvas until the browser restores
    // it). render_frame() below then leaves the dirty flag SET, so the UI
    // repaints by itself on the first frame after the restore.
    bool gpu_surface_available() const { return skia_ && skia_->is_available(); }

    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
    }

    void set_idle_callback(std::function<void()> cb) override {
        idle_callback_ = std::move(cb);
    }

    void request_close() override {
        if (render_loop_owned_) render_loop_owned_->stop();
        running_ = false;
        hide();
        if (close_callback_) close_callback_();
    }

    void invalidate_input_state() override { router_.invalidate(); }

    void set_design_viewport(float design_w, float design_h) override {
        design_w_ = design_w > 0 ? design_w : 0.0f;
        design_h_ = design_h > 0 ? design_h : 0.0f;
        sync_router_mapping();
        mark_dirty();
    }

    bool design_viewport_transform(float& sx, float& sy, float& tx,
                                   float& ty) const override {
        return router_.mapping().transform(sx, sy, tx, ty);
    }

    render::GpuSurface* gpu_surface() const override { return nullptr; }

    // ── Capture ─────────────────────────────────────────────────────────

    std::vector<uint8_t> capture_png() override { return capture_back_buffer_png(); }

    /// Renders a fresh frame and reads it back. SUBMIT BEFORE READBACK: the
    /// pixels only exist once SkiaSurface::end_frame() has flushed and submitted
    /// the recorded work — reading first samples the previous frame (or
    /// uninitialized memory on the first), the determinism bug HeadlessSurface
    /// documents at render_rgba().
    std::vector<uint8_t> capture_back_buffer_png() override {
        if (!skia_ || !skia_->is_available()) return {};
        render::HeadlessSurface::Rgba rgba;
        if (!render_and_read(rgba)) return {};
        return render::HeadlessSurface::encode_png(rgba);
    }

    /// The canvas element's CSS size changed (window resize / layout / dpr
    /// change). Re-sizes the backing store, the Skia surface, and the root.
    void handle_resize(float css_width, float css_height) {
        const float dpr = device_pixel_ratio();
        if (css_width == css_width_ && css_height == css_height_ && dpr == dpr_)
            return;

        css_width_ = css_width;
        css_height_ = css_height;
        dpr_ = dpr;

        const auto backing = compute_backing_store(css_width_, css_height_, dpr_);
        set_canvas_size(canvas_selector_, backing, css_width_, css_height_);
        if (skia_) {
            skia_->resize(static_cast<uint32_t>(css_width_),
                          static_cast<uint32_t>(css_height_),
                          backing.scale_factor);
        }
        sync_router_mapping();

        // Relayout synchronously so an input handler that fires before the next
        // frame hit-tests against the new geometry.
        root_.set_bounds(root_bounds());
        root_.layout_children();
        if (resize_callback_) {
            resize_callback_(static_cast<uint32_t>(std::max(0.0f, css_width_)),
                             static_cast<uint32_t>(std::max(0.0f, css_height_)));
        }
        mark_dirty();
    }

private:
    void create_surface() {
        dpr_ = device_pixel_ratio();
        const auto backing = compute_backing_store(css_width_, css_height_, dpr_);
        set_canvas_size(canvas_selector_, backing, css_width_, css_height_);

        // The Skia surface takes CSS (logical) dimensions plus the scale factor
        // and applies canvas->scale(scale, scale) itself, so the view tree lays
        // out and paints in CSS pixels while the backing store is device-sized.
        render::SkiaSurface::WebGlConfig config{};
        config.canvas_selector = canvas_selector_.c_str();
        config.width = static_cast<uint32_t>(std::max(0.0f, css_width_));
        config.height = static_cast<uint32_t>(std::max(0.0f, css_height_));
        config.scale_factor = backing.scale_factor;
        skia_ = render::SkiaSurface::create_webgl(config);
    }

    Rect root_bounds() const {
        if (design_w_ > 0 && design_h_ > 0)
            return {0, 0, design_w_, design_h_};
        return {0, 0, css_width_, css_height_};
    }

    void sync_router_mapping() {
        ViewportMapping mapping;
        mapping.css_width = css_width_;
        mapping.css_height = css_height_;
        mapping.design_width = design_w_;
        mapping.design_height = design_h_;
        router_.set_mapping(mapping);
    }

    void on_frame() {
        drain_dispatcher_tasks();
        if (idle_callback_) idle_callback_();

        const double now = emscripten_get_now();
        const float dt = last_frame_ms_ > 0
                             ? static_cast<float>((now - last_frame_ms_) / 1000.0)
                             : 0.0f;
        last_frame_ms_ = now;
        frame_clock_.tick(dt);

        render_frame();

        // A LOST WebGL context makes render_frame() a no-op (begin_frame()
        // returns nullptr, so the dirty flag stays set). Keep asking for frames
        // while it is lost: an idle tree would otherwise stop requesting them and
        // the canvas would stay frozen even after the browser restored the
        // context. `skia_ == nullptr` (the surface never came up at all) is NOT
        // this case and must not spin.
        const bool surface_lost = skia_ && !skia_->is_available();

        // Animations and live meters need the next frame without an external
        // trigger; an idle tree costs nothing because the loop is demand-driven.
        if (render_loop_owned_ &&
            (surface_lost || needs_continuous_frames(&root_) ||
             frame_clock_.has_active_subscribers())) {
            render_loop_owned_->request_frame();
        }
    }

    void paint_scene(canvas::Canvas& canvas) {
        float sx = 1, sy = 1, tx = 0, ty = 0;
        const bool has_viewport = router_.mapping().transform(sx, sy, tx, ty);

        root_.set_bounds(root_bounds());
        root_.layout_children();

        canvas.set_fill_color(canvas::Color::rgba8(30, 30, 46));
        canvas.fill_rect(0, 0, css_width_, css_height_);

        if (has_viewport) {
            // Overlays draw in ROOT coordinates and the input path inverse-maps
            // into root space, so they must paint INSIDE the design transform or
            // they land misaligned and un-clickable at any non-design size.
            const int saved = canvas.save_count();
            canvas.save();
            canvas.translate(tx, ty);
            canvas.scale(sx, sy);
            root_.paint_all(canvas);
            View::paint_overlays(canvas, &root_);
            canvas.restore_to_count(saved);
        } else {
            root_.paint_all(canvas);
            View::paint_overlays(canvas, &root_);
        }
    }

    bool render_frame() {
        if (!skia_) return false;
        // RE-ENTRANCY GUARD. A view that calls request_repaint() from inside paint() — any
        // continuously animating editor, e.g. SuperConvolver's living field — routes through
        // WindowHost::mark_dirty() -> schedule_repaint(). Before the rAF RenderLoop is
        // attached (run_event_loop() calls show() -> mark_dirty() BEFORE it creates the loop),
        // or on any build where schedule_repaint() falls back to a direct repaint(), that path
        // calls render_frame() SYNCHRONOUSLY — re-entering paint from within paint and
        // recursing until the JS stack overflows (the crash then lands in whatever draw the
        // over-deep stack happens to hit, e.g. a gradient/texture upload, which is a red
        // herring). Swallow the nested call and leave the surface dirty so the NEXT real frame
        // repaints — the animation keeps flowing, one frame per tick, never nested.
        if (rendering_) { reentrant_dirty_ = true; return false; }
        rendering_ = true;
        auto* canvas = skia_->begin_frame();
        if (!canvas) { rendering_ = false; return false; }
        paint_scene(*canvas);
        // Ganesh flushes and submits here; the browser composites the canvas
        // element, so there is no separate present step.
        skia_->end_frame();
        clear_pending_dirty();
        rendering_ = false;
        // A repaint requested DURING this paint (see the guard above) is honoured on the NEXT
        // frame, not by re-entering — one frame per tick, never nested.
        if (reentrant_dirty_) {
            reentrant_dirty_ = false;
            if (render_loop_owned_) render_loop_owned_->request_frame();
        }
        return true;
    }
    bool rendering_ = false;
    bool reentrant_dirty_ = false;

    bool render_and_read(render::HeadlessSurface::Rgba& out) {
        if (!skia_) return false;
        auto* canvas = skia_->begin_frame();
        if (!canvas) return false;
        paint_scene(*canvas);
        skia_->end_frame();
        clear_pending_dirty();
        return skia_->read_current_rgba(out.pixels, out.width, out.height);
    }

    // ── MainThreadDispatcher backend ────────────────────────────────────

    struct DispatcherQueueState {
        mutable std::mutex mutex;
        std::deque<events::Task> tasks;
        bool accepting = true;
    };

    // A queued task also has to WAKE the host: the rAF loop is demand-driven,
    // so a task posted while the UI is idle would otherwise sit in the queue
    // until some unrelated event happened to request a frame.
    static bool enqueue_and_wake(const std::shared_ptr<DispatcherQueueState>& queue,
                                 const std::shared_ptr<std::atomic<bool>>& alive,
                                 BrowserWindowHost* host, events::Task task) {
        {
            std::lock_guard lock(queue->mutex);
            if (!queue->accepting) return false;
            queue->tasks.push_back(std::move(task));
        }
        if (alive->load(std::memory_order_acquire)) host->mark_dirty();
        return true;
    }

    struct DelayedPost {
        std::weak_ptr<DispatcherQueueState> queue;
        std::weak_ptr<std::atomic<bool>> alive;
        BrowserWindowHost* host = nullptr;
        events::Task task;
    };

    void register_dispatcher() {
        shutdown_dispatcher();

        auto queue = std::make_shared<DispatcherQueueState>();
        auto alive = alive_;
        auto* host = this;
        auto loop_thread_id = std::this_thread::get_id();
        dispatcher_token_ = events::MainThreadDispatcher::register_backend({
            [queue, alive, host](events::Task task) {
                if (!task) return false;
                return enqueue_and_wake(queue, alive, host, std::move(task));
            },
            [loop_thread_id] {
                return std::this_thread::get_id() == loop_thread_id;
            },
            // setTimeout-paced post: a caller that wants a paced main-thread
            // poll gets one instead of busy-reposting every frame.
            [queue, alive, host](events::Task task, int delay_ms) {
                if (!task) return false;
                auto* boxed = new DelayedPost{queue, alive, host, std::move(task)};
                emscripten_async_call(
                    [](void* raw) {
                        std::unique_ptr<DelayedPost> post(
                            static_cast<DelayedPost*>(raw));
                        auto queue_ref = post->queue.lock();
                        auto alive_ref = post->alive.lock();
                        if (!queue_ref || !alive_ref) return;
                        enqueue_and_wake(queue_ref, alive_ref, post->host,
                                         std::move(post->task));
                    },
                    boxed, std::max(0, delay_ms));
                return true;
            },
        });
        dispatcher_queue_ = std::move(queue);
    }

    void drain_dispatcher_tasks() {
        if (!dispatcher_queue_) return;

        std::deque<events::Task> tasks;
        {
            std::lock_guard lock(dispatcher_queue_->mutex);
            tasks.swap(dispatcher_queue_->tasks);
        }
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            if (task) task();
        }
    }

    void shutdown_dispatcher() {
        events::MainThreadDispatcher::unregister_backend(dispatcher_token_);
        dispatcher_token_ = 0;

        if (!dispatcher_queue_) return;
        {
            std::lock_guard lock(dispatcher_queue_->mutex);
            dispatcher_queue_->accepting = false;
        }
        drain_dispatcher_tasks();
        dispatcher_queue_.reset();
    }

    View& root_;
    WindowOptions options_;
    std::string canvas_selector_;
    WebInputRouter router_;

    std::unique_ptr<render::SkiaSurface> skia_;
    std::unique_ptr<render::RenderLoop> render_loop_owned_;
    FrameClock frame_clock_;

    float css_width_ = 0;
    float css_height_ = 0;
    float dpr_ = 1.0f;
    float design_w_ = 0;
    float design_h_ = 0;
    bool visible_ = false;
    bool running_ = false;
    double last_frame_ms_ = 0;

    std::function<void()> close_callback_;
    std::function<void()> idle_callback_;
    ResizeCallback resize_callback_;

    std::shared_ptr<DispatcherQueueState> dispatcher_queue_;
    events::MainThreadDispatcher::Token dispatcher_token_ = 0;

    // Cleared in the destructor: a setTimeout-paced post that fires after the
    // host is gone must not wake a dangling `this`.
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
};

}  // namespace

// The resize listener in web_input.cpp reports the canvas's new CSS size here.
void notify_browser_host_resized(float css_width, float css_height) {
    if (g_live_host) g_live_host->handle_resize(css_width, css_height);
}

bool browser_host_gpu_available() {
    return g_live_host && g_live_host->gpu_surface_available();
}

void install_browser_window_host(std::string canvas_selector) {
    WindowHost::set_factory(
        [selector = std::move(canvas_selector)](
            View& root, const WindowOptions& options) -> std::unique_ptr<WindowHost> {
            auto host = std::make_unique<BrowserWindowHost>(root, options, selector);
            g_live_host = host.get();
            return host;
        });
}

}  // namespace pulp::view::web

#endif  // __EMSCRIPTEN__
