#include <pulp/view/sdl_window_host.hpp>

#ifdef PULP_HAS_SDL3

#include <pulp/canvas/canvas.hpp>
#include <SDL3/SDL.h>
#include <iostream>

// On macOS, use CoreGraphics canvas for rendering into SDL surface
#ifdef __APPLE__
#include <pulp/canvas/cg_canvas.hpp>
#endif

namespace pulp::view {

class SdlWindowHostImpl : public SdlWindowHost {
public:
    SdlWindowHostImpl(View& root, Options options)
        : root_(root), options_(std::move(options)) {

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
                    default:
                        break;
                }
            }

            if (needs_repaint_) {
                render_frame();
                needs_repaint_ = false;
            }

            SDL_Delay(16); // ~60fps
        }

        if (close_callback_) close_callback_();
    }

    // ── Platform feature overrides (SDL3, Linux/Windows) ──────────────

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
