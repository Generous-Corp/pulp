// RenderLoop platform implementations.
//
// Each platform uses its native vsync mechanism for frame-paced rendering;
// the shared RenderLoopState (render_loop_state.hpp) is the platform-
// agnostic dirty-flag coalescer that turns N request_frame() calls between
// two vblanks into exactly one callback invocation.
//
//   macOS   — MacRenderLoop      : CVDisplayLink           (this file)
//   iOS     — IOSRenderLoop      : CADisplayLink           (render_loop_apple.mm)
//   Android — AndroidRenderLoop  : AChoreographer          (render_loop_android.cpp)
//   Windows — WindowsRenderLoop  : DwmFlush vblank wait    (this file)
//   Browser — EmscriptenRenderLoop : requestAnimationFrame (render_loop_emscripten.cpp)
//   Linux   — TimerRenderLoop    : conscious timer fallback (this file)
//
// RenderLoopState owns the shared coalescing behavior; native subclasses only
// provide the pacing source.
//
// TimerRenderLoop is compiled on every platform EXCEPT Emscripten: it drives
// its callback from a std::thread, and constructing one in a wasm build
// without pthreads throws std::system_error. The browser has a real vblank
// source (rAF), so it needs no timer fallback and must not carry a class that
// cannot be instantiated there.

#include <pulp/render/render_loop.hpp>
#include <pulp/runtime/log.hpp>
#include "render_loop_state.hpp"
#include <thread>
#include <chrono>

#if defined(__APPLE__) && !defined(PULP_RENDER_LOOP_FORCE_TIMER)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreVideo/CVDisplayLink.h>
#include <dispatch/dispatch.h>
#endif
#endif

#if defined(_WIN32) && !defined(PULP_RENDER_LOOP_FORCE_TIMER)
#include <windows.h>
#include <dwmapi.h>
#endif

namespace pulp::render {

const char* render_loop_backend_name(RenderLoopBackend backend) {
    switch (backend) {
        case RenderLoopBackend::cv_display_link: return "CVDisplayLink";
        case RenderLoopBackend::ca_display_link: return "CADisplayLink";
        case RenderLoopBackend::choreographer:   return "AChoreographer";
        case RenderLoopBackend::dwm_flush:       return "DwmFlush";
        case RenderLoopBackend::raf:             return "requestAnimationFrame";
        case RenderLoopBackend::timer:           return "timer";
    }
    return "unknown";
}

// ── macOS: CVDisplayLink ────────────────────────────────────────────────

#if defined(__APPLE__) && !defined(PULP_RENDER_LOOP_FORCE_TIMER) && TARGET_OS_OSX

class MacRenderLoop : public RenderLoop {
public:
    ~MacRenderLoop() override { stop(); }

    void start(FrameCallback on_frame) override {
        if (!state_.start()) {
            return;
        }
        callback_ = std::move(on_frame);

        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, &display_link_callback, this);
        CVDisplayLinkStart(display_link_);
    }

    void stop() override {
        state_.stop();
        if (display_link_) {
            CVDisplayLinkStop(display_link_);
            CVDisplayLinkRelease(display_link_);
            display_link_ = nullptr;
        }
    }

    void request_frame() override {
        state_.request_frame();
    }

    bool is_running() const override { return state_.is_running(); }

    RenderLoopBackend backend() const override {
        return RenderLoopBackend::cv_display_link;
    }

private:
    CVDisplayLinkRef display_link_ = nullptr;
    FrameCallback callback_;
    RenderLoopState state_;

    static CVReturn display_link_callback(CVDisplayLinkRef, const CVTimeStamp*,
        const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* ctx) {
        auto* self = static_cast<MacRenderLoop*>(ctx);
        if (self->state_.consume_frame_request()) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->state_.is_running() && self->callback_) {
                    self->callback_();
                }
            });
        }
        return kCVReturnSuccess;
    }
};

#endif // macOS

// ── Windows: DwmFlush (DWM-composed vblank wait) ────────────────────────
//
// DwmFlush() blocks until the DWM compositor's next composition pass,
// which is paced to the display's vblank. Unlike a present-paced waitable
// swapchain it needs no swapchain/window handle, so it works from the
// RenderLoop factory seam without a native_handle. If DwmFlush() fails
// (no DWM — Server Core, remote session, or a future headless config) the
// loop transparently degrades to the ~60 Hz timer cadence for that frame.

#if defined(_WIN32) && !defined(PULP_RENDER_LOOP_FORCE_TIMER)

class WindowsRenderLoop : public RenderLoop {
public:
    ~WindowsRenderLoop() override { stop(); }

    void start(FrameCallback on_frame) override {
        if (!state_.start()) {
            return;
        }
        callback_ = std::move(on_frame);

        thread_ = std::thread([this]() {
            using namespace std::chrono;
            const auto fallback_interval = microseconds(16667); // ~60Hz
            while (state_.is_running()) {
                // Block until the next DWM composition (vblank-paced).
                // S_OK means we waited on a real vblank; any failure
                // (DWM disabled / unavailable) falls back to a timed
                // sleep so the loop still makes forward progress.
                const HRESULT hr = DwmFlush();
                backend_tracker_.note_wait_result(hr == S_OK);
                if (hr != S_OK && state_.is_running()) {
                    std::this_thread::sleep_for(fallback_interval);
                }
                if (state_.consume_frame_request() && state_.is_running()) {
                    if (callback_) callback_();
                }
            }
        });
    }

    void stop() override {
        state_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void request_frame() override {
        state_.request_frame();
    }

    bool is_running() const override { return state_.is_running(); }

    // Reports timer once DwmFlush() has failed, so callers using
    // render_loop_backend_is_vsync() can detect the fallback.
    RenderLoopBackend backend() const override {
        return backend_tracker_.effective_backend();
    }

private:
    std::thread thread_;
    FrameCallback callback_;
    RenderLoopState state_;
    DwmBackendTracker backend_tracker_;
};

#endif // Windows

// ── Generic: Timer-based fallback ───────────────────────────────────────
//
// Used on Linux and any other platform without a wired vblank source, and
// by headless unit tests via PULP_RENDER_LOOP_FORCE_TIMER. This is a
// CONSCIOUS fallback, not a real vblank lock: Linux real pacing needs an
// X11 (GLX_OML_sync_control / DRM vblank) or Wayland (frame-callback)
// presentation object that the RenderLoop factory does not own today. The
// factory seam below is the single point where a real LinuxRenderLoop
// drops in once that surface exists — no consumer code changes.

#if !defined(__EMSCRIPTEN__)

class TimerRenderLoop : public RenderLoop {
public:
    ~TimerRenderLoop() override { stop(); }

    void start(FrameCallback on_frame) override {
        if (!state_.start()) {
            return;
        }
        callback_ = std::move(on_frame);

        thread_ = std::thread([this]() {
            using namespace std::chrono;
            auto frame_interval = microseconds(16667); // ~60Hz
            while (state_.is_running()) {
                auto start = steady_clock::now();
                if (state_.consume_frame_request() && state_.is_running()) {
                    if (callback_) callback_();
                }
                auto elapsed = steady_clock::now() - start;
                auto sleep_time = frame_interval - duration_cast<microseconds>(elapsed);
                if (sleep_time.count() > 0) {
                    std::this_thread::sleep_for(sleep_time);
                }
            }
        });
    }

    void stop() override {
        state_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void request_frame() override {
        state_.request_frame();
    }

    bool is_running() const override { return state_.is_running(); }

    RenderLoopBackend backend() const override {
        return RenderLoopBackend::timer;
    }

private:
    std::thread thread_;
    FrameCallback callback_;
    RenderLoopState state_;
};

#endif // !__EMSCRIPTEN__

// ── Factory ─────────────────────────────────────────────────────────────
//
// Single seam for backend selection. Platform subclasses defined in their
// own translation units (iOS / Android) are constructed via the forward-
// declared factory helpers below so this file stays C++-only.

#if defined(__APPLE__) && !defined(PULP_RENDER_LOOP_FORCE_TIMER) && TARGET_OS_IPHONE
// Defined in render_loop_apple.mm — owns a CADisplayLink.
std::unique_ptr<RenderLoop> make_ios_render_loop();
#endif

#if defined(__ANDROID__) && !defined(PULP_RENDER_LOOP_FORCE_TIMER)
// Defined in render_loop_android.cpp — owns an AChoreographer registration.
std::unique_ptr<RenderLoop> make_android_render_loop();
#endif

#if defined(__EMSCRIPTEN__)
// Defined in render_loop_emscripten.cpp — owns a requestAnimationFrame loop.
std::unique_ptr<RenderLoop> make_emscripten_render_loop();
#endif

std::unique_ptr<RenderLoop> RenderLoop::create(void* native_handle) {
    (void)native_handle;
    // The browser arm precedes PULP_RENDER_LOOP_FORCE_TIMER because there is
    // no timer loop to force there — see the TimerRenderLoop guard above.
#if defined(__EMSCRIPTEN__)
    return make_emscripten_render_loop();
#elif defined(PULP_RENDER_LOOP_FORCE_TIMER)
    return std::make_unique<TimerRenderLoop>();
#elif defined(__APPLE__) && TARGET_OS_OSX
    return std::make_unique<MacRenderLoop>();
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return make_ios_render_loop();
#elif defined(__ANDROID__)
    return make_android_render_loop();
#elif defined(_WIN32)
    return std::make_unique<WindowsRenderLoop>();
#else
    // Linux and any other platform: conscious timer fallback.
    return std::make_unique<TimerRenderLoop>();
#endif
}

std::unique_ptr<RenderLoop> RenderLoop::create_timer_loop() {
#if defined(__EMSCRIPTEN__)
    // No thread-backed loop exists in a wasm build; rAF is the only pacing
    // source. Callers get a working loop rather than a std::system_error.
    return make_emscripten_render_loop();
#else
    // TimerRenderLoop is compiled on every other platform — no native run loop
    // needed, so this is the deterministic choice for headless tests.
    return std::make_unique<TimerRenderLoop>();
#endif
}

} // namespace pulp::render
