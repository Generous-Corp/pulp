#include <pulp/view/sdl_window_host.hpp>

#ifdef PULP_HAS_SDL3

#include <pulp/canvas/canvas.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/host_frame_pump.hpp>
#include <SDL3/SDL.h>
#include <cstdint>  // uintptr_t (X11 Window id for outbound file drag)
#include <string>
#include <vector>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// On macOS, use CoreGraphics canvas for rendering into SDL surface
#ifdef __APPLE__
#include <pulp/canvas/cg_canvas.hpp>
#endif

namespace pulp::view {

class SdlWindowHostImpl : public SdlWindowHost {
public:
    SdlWindowHostImpl(View& root, Options options)
        : root_(root), options_(std::move(options)) {

        // The SDL host (the Windows / Linux standalone window) previously owned
        // no FrameClock and advanced no animation at all: widget hover/thumb
        // animations, CSS timelines, and FrameClock subscribers were frozen on
        // those platforms. It now runs the same measured-dt frame-pump contract
        // as the Apple hosts (host_frame_pump.hpp).
        root_.set_frame_clock(&frame_clock_);

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::cerr << "[pulp] SDL_Init failed: " << SDL_GetError() << "\n";
            return;
        }

        window_ = SDL_CreateWindow(
            options_.title.c_str(),
            static_cast<int>(options_.width),
            static_cast<int>(options_.height),
            (options_.resizable ? SDL_WINDOW_RESIZABLE : 0) | SDL_WINDOW_HIGH_PIXEL_DENSITY
        );

        if (!window_) {
            std::cerr << "[pulp] SDL_CreateWindow failed: " << SDL_GetError() << "\n";
            return;
        }

        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            std::cerr << "[pulp] SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        }
    }

    ~SdlWindowHostImpl() {
        root_.set_frame_clock(nullptr);
        shutdown_dispatcher();
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    void show() {
        if (window_) SDL_ShowWindow(window_);
    }

    void hide() {
        if (window_) SDL_HideWindow(window_);
    }

    void repaint() {
        needs_repaint_ = true;
    }

    void set_close_callback(std::function<void()> cb) {
        close_callback_ = std::move(cb);
    }

    void run_event_loop() {
        if (!window_ || !renderer_) return;

        show();
        register_dispatcher();

        // Attach the OS-native accessibility provider for this root. On Linux
        // this exports the AT-SPI accessible tree over D-Bus; the provider's
        // inbound method calls must be pumped each loop iteration (see
        // accessibility_pump below) or a connected screen reader hangs. On
        // platforms with a callback-driven provider (none route through the SDL
        // host today) init_accessibility / accessibility_pump are honest no-ops.
        // The handle is sentinel-or-null when no a11y bus is reachable; pump and
        // shutdown both tolerate that.
        accessibility_handle_ = init_accessibility(root_, nullptr);

        bool running = true;
        needs_repaint_ = true;

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_RESIZED:
                    case SDL_EVENT_WINDOW_EXPOSED:
                        needs_repaint_ = true;
                        break;
                    // Native drag-and-drop. SDL3 abstracts Win OLE / Linux XDND
                    // (and macOS) into normalized drop events and delivers each
                    // dropped file as its own SDL_EVENT_DROP_FILE, bracketed by
                    // BEGIN/COMPLETE. We accumulate the files across the sequence
                    // and route the batch into the view tree via the shared
                    // dispatch core (drag_drop.cpp).
                    case SDL_EVENT_DROP_BEGIN:
                        pending_drop_files_.clear();
                        pending_drop_pos_ = window_to_root(event.drop.x, event.drop.y);
                        break;
                    case SDL_EVENT_DROP_FILE:
                        if (event.drop.data) pending_drop_files_.emplace_back(event.drop.data);
                        pending_drop_pos_ = window_to_root(event.drop.x, event.drop.y);
                        break;
                    case SDL_EVENT_DROP_TEXT:
                        if (event.drop.data) {
                            DropData d;
                            d.type = DropData::Type::text;
                            d.text = event.drop.data;
                            dispatch_drop(root_, drag_session_, d,
                                          window_to_root(event.drop.x, event.drop.y));
                            needs_repaint_ = true;
                        }
                        break;
                    case SDL_EVENT_DROP_POSITION:
                        // Hover feedback while dragging over the window. The file
                        // list is only known once DROP_FILE events arrive, so this
                        // updates highlight state only when we already have paths.
                        if (!pending_drop_files_.empty()) {
                            DropData d;
                            d.type = DropData::Type::files;
                            d.file_paths = pending_drop_files_;
                            dispatch_drag_move(root_, drag_session_, d,
                                               window_to_root(event.drop.x, event.drop.y));
                            needs_repaint_ = true;
                        }
                        break;
                    case SDL_EVENT_DROP_COMPLETE:
                        if (!pending_drop_files_.empty()) {
                            DropData d;
                            d.type = DropData::Type::files;
                            d.file_paths = std::move(pending_drop_files_);
                            dispatch_drop(root_, drag_session_, d, pending_drop_pos_);
                            pending_drop_files_.clear();
                            needs_repaint_ = true;
                        } else {
                            dispatch_drag_exit(root_, drag_session_);
                        }
                        break;
                    default:
                        break;
                }
            }

            drain_dispatcher_tasks();

            // Service inbound accessibility IPC (Linux AT-SPI registry / Orca
            // method calls on the exported View tree). Cheap when no assistive
            // tech is connected; a no-op on the sentinel handle.
            accessibility_pump(accessibility_handle_);

            // One measured dt per loop iteration, from SDL's monotonic
            // performance counter — never the 16 ms the delay below ASKS for
            // (the loop body takes time, the OS oversleeps, and a busy frame
            // slips). begin_host_frame pumps the activity probes and decides
            // whether this iteration composites; advance_host_frame then drives
            // the FrameClock, widget animations, and CSS timelines by that ONE dt.
            const double now =
                static_cast<double>(SDL_GetPerformanceCounter()) /
                static_cast<double>(SDL_GetPerformanceFrequency());
            const auto tick = begin_host_frame(&root_, frame_clock_, frame_pump_,
                                               now, needs_repaint_);
            if (tick.should_render) {
                advance_host_frame(&root_, frame_clock_, tick.dt);
                render_frame();
                needs_repaint_ = false;
            }

            SDL_Delay(16); // ~60fps
        }

        shutdown_accessibility(accessibility_handle_);
        accessibility_handle_ = nullptr;
        shutdown_dispatcher();
        if (close_callback_) close_callback_();
    }

    // ── Platform feature overrides (SDL3, Linux/Windows) ──────────────

    // Native window handle for outbound file drag (View::start_file_drag's free
    // begin_file_drag backend). HWND on Windows — the OLE DoDragDrop backend
    // ignores its value, but the cross-platform call site guards on non-null;
    // the X11 Window id on Linux (the XDND source backend opens its own Display
    // from it). Wayland / unsupported → nullptr, so the drag degrades to false.
    void* native_content_view_handle() const override {
        if (!window_) return nullptr;
        SDL_PropertiesID props = SDL_GetWindowProperties(window_);
#if defined(_WIN32)
        return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                      nullptr);
#elif defined(__linux__)
        return reinterpret_cast<void*>(static_cast<uintptr_t>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
#else
        return nullptr;
#endif
    }

    void set_mouse_relative_mode(bool enabled) {
        if (window_) SDL_SetWindowRelativeMouseMode(window_, enabled);
    }

    float dpi_scale() const {
        if (!window_) return 1.0f;
        return SDL_GetWindowDisplayScale(window_);
    }

    Size max_dimensions() const {
        SDL_DisplayID display = window_ ? SDL_GetDisplayForWindow(window_) : SDL_GetPrimaryDisplay();
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
        if (mode) return {static_cast<float>(mode->w), static_cast<float>(mode->h)};
        return {1920, 1080};
    }

    void set_always_on_top(bool on_top) {
        if (window_) SDL_SetWindowAlwaysOnTop(window_, on_top);
    }

    void set_fixed_aspect_ratio(float ratio) {
        if (window_ && ratio > 0)
            SDL_SetWindowAspectRatio(window_, ratio, ratio);
    }

    bool is_visible() const {
        return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_HIDDEN) == 0;
    }

private:
    View& root_;
    Options options_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::function<void()> close_callback_;
    bool needs_repaint_ = false;
    FrameClock frame_clock_;
    // Measured-dt source for the loop (SDL_GetPerformanceCounter).
    HostFramePump frame_pump_;

    // Opaque OS-accessibility provider handle (Linux AT-SPI today). Owned for
    // the lifetime of run_event_loop(): created after the window is shown,
    // pumped each iteration, torn down on exit. Null / sentinel when no
    // provider attaches (honest-fail), which pump + shutdown both tolerate.
    void* accessibility_handle_ = nullptr;

    // Files accumulated across an in-flight native drag-drop sequence
    // (SDL_EVENT_DROP_BEGIN … DROP_FILE* … DROP_COMPLETE) and the last reported
    // drop position, both in window coordinates (== root coords here).
    std::vector<std::string> pending_drop_files_;
    Point pending_drop_pos_{0, 0};
    // Per-window drop hover state (owned here, not a process global).
    DragSession drag_session_;

    struct DispatcherQueueState {
        mutable std::mutex mutex;
        std::deque<events::Task> tasks;
        bool accepting = true;
    };

    std::shared_ptr<DispatcherQueueState> dispatcher_queue_;
    events::MainThreadDispatcher::Token dispatcher_token_ = 0;

    // Map SDL window coordinates → root-view coordinates. The single chokepoint
    // for all drop-event coords. render_frame() lays the root tree out at the
    // full window size with no design-viewport / HiDPI scale, so this is identity
    // today. When the SDL host gains a design-viewport or per-monitor scale
    // (W8/L9 HiDPI), apply that inverse transform HERE — and route mouse events
    // through the same helper — so drops keep landing under the cursor.
    Point window_to_root(float window_x, float window_y) const {
        return {window_x, window_y};
    }

    void register_dispatcher() {
        shutdown_dispatcher();

        auto queue = std::make_shared<DispatcherQueueState>();
        auto loop_thread_id = std::this_thread::get_id();
        auto token = events::MainThreadDispatcher::register_backend({
            [queue](events::Task task) {
                if (!task)
                    return false;
                std::lock_guard lock(queue->mutex);
                if (!queue->accepting)
                    return false;
                queue->tasks.push_back(std::move(task));
                return true;
            },
            [loop_thread_id] {
                return std::this_thread::get_id() == loop_thread_id;
            },
        });

        dispatcher_queue_ = std::move(queue);
        dispatcher_token_ = token;
    }

    void drain_dispatcher_tasks() {
        if (!dispatcher_queue_)
            return;

        std::deque<events::Task> tasks;
        {
            std::lock_guard lock(dispatcher_queue_->mutex);
            tasks.swap(dispatcher_queue_->tasks);
        }

        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            if (task)
                task();
        }
    }

    void shutdown_dispatcher() {
        events::MainThreadDispatcher::unregister_backend(dispatcher_token_);
        dispatcher_token_ = 0;

        if (!dispatcher_queue_)
            return;

        {
            std::lock_guard lock(dispatcher_queue_->mutex);
            dispatcher_queue_->accepting = false;
        }
        drain_dispatcher_tasks();
        dispatcher_queue_.reset();
    }

    void render_frame() {
        if (!renderer_) return;

        int w, h;
        SDL_GetWindowSize(window_, &w, &h);

        // Use SDL renderer for basic clearing and present
        SDL_SetRenderDrawColor(renderer_, 30, 30, 46, 255);
        SDL_RenderClear(renderer_);

        // For now, use RecordingCanvas to paint the view tree
        // (real GPU rendering will use WebGPU canvas when Skia is integrated)
        // The view tree layout and paint still work — just not visible on screen yet
        // via SDL. The CoreGraphics path (WindowHost) handles visible rendering on macOS.
        canvas::RecordingCanvas recording_canvas;
        root_.set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
        root_.layout_children();
        root_.paint_all(recording_canvas);

        // Draw a simple "rendering active" indicator using SDL
        SDL_FRect rect = {10, 10, 100, 20};
        SDL_SetRenderDrawColor(renderer_, 100, 180, 250, 255);
        SDL_RenderFillRect(renderer_, &rect);

        SDL_RenderPresent(renderer_);
    }
};

std::unique_ptr<SdlWindowHost> SdlWindowHost::create(View& root, Options options) {
    return std::make_unique<SdlWindowHostImpl>(root, std::move(options));
}

} // namespace pulp::view

#else // !PULP_HAS_SDL3

namespace pulp::view {

std::unique_ptr<SdlWindowHost> SdlWindowHost::create(View&, Options) {
    return nullptr;
}

} // namespace pulp::view

#endif
