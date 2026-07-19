#pragma once

// RenderLoop abstraction for frame-paced GPU rendering.
//
// Platform-specific implementations drive the render callback at the
// display's refresh rate (vblank). The canonical pattern is *"set a dirty
// flag, repaint on the next frame"* rather than periodic polling — call
// request_frame() to mark dirty and the loop fires the callback exactly
// once on the next vblank, coalescing any number of intervening requests.
//
//   macOS   : CVDisplayLink                       (real vblank)
//   iOS     : CADisplayLink                       (real vblank)
//   Android : AChoreographer                      (real vblank)
//   Windows : DwmFlush (DWM-composed vblank wait)  (real vblank)
//   Browser : requestAnimationFrame               (real vblank)
//   Linux   : timer fallback                      (conscious fallback —
//             a real X11/Wayland present-sync source drops in behind the
//             same factory seam when available; see render_loop.cpp)
//   Other   : timer fallback                      (~60 Hz)
//
// The VBlank-locked safe-repaint pattern keeps dirty-frame coalescing in
// RenderLoopState (render_loop_state.hpp), which is platform-agnostic and
// unit-tested; each platform subclass only owns the native vsync source.
//
// Wiring status (important — do not mistake these backends for dead code):
// only the WEB host (window_host_web.cpp, emscripten) currently drives its
// frames through RenderLoop::create() in production. The macOS / Windows /
// iOS / Android / timer backends are fully implemented and unit-tested but are
// NOT instantiated by their native hosts today — those hosts hand-roll their
// vsync source directly (macOS drives CVDisplayLink inside window_host_mac.mm;
// iOS drives CADisplayLink inside window_host_ios.mm; Android via
// AChoreographer). This class is therefore a *reusable vsync abstraction for
// host implementors* plus the one live web consumer, not an unused facade.
// Consolidating the native hosts onto these backends (dedup, one vsync
// abstraction) is a deliberate future slice, not a bug — it is intentionally
// deferred because the hand-rolled paths carry host-specific lifecycle
// (display-link start/stop on show/hide, secondary-window display links) that
// a migration must preserve.

#include <functional>
#include <memory>

namespace pulp::render {

using FrameCallback = std::function<void()>;

/// Which concrete backend RenderLoop::create() selected. Exposed so callers
/// (and tests) can introspect whether a real vblank source or the timer
/// fallback is in use without depending on platform defines.
enum class RenderLoopBackend {
    cv_display_link,   ///< macOS — real vblank.
    ca_display_link,   ///< iOS — real vblank.
    choreographer,     ///< Android — real vblank.
    dwm_flush,         ///< Windows — DWM-composed vblank wait.
    raf,               ///< Browser — requestAnimationFrame (compositor vblank).
    timer,             ///< Conscious ~60 Hz timer fallback (Linux/other).
};

/// Human-readable name for a backend (for logs / diagnostics).
const char* render_loop_backend_name(RenderLoopBackend backend);

/// True if the backend is locked to a real hardware/compositor vblank
/// signal (as opposed to the conscious timer fallback).
constexpr bool render_loop_backend_is_vsync(RenderLoopBackend backend) {
    return backend != RenderLoopBackend::timer;
}

class RenderLoop {
public:
    // Create a platform-appropriate render loop.
    // native_handle: optional window/view handle (NSView*, HWND, SDL_Window*).
    // Defining PULP_RENDER_LOOP_FORCE_TIMER forces the timer fallback on
    // every platform — used by headless unit tests.
    static std::unique_ptr<RenderLoop> create(void* native_handle = nullptr);

    // Create the conscious ~60 Hz timer-backed render loop explicitly, on
    // any platform. Unlike create(), the result drives its callback from a
    // worker thread and never depends on a native run loop / compositor, so
    // it is deterministic in headless contexts. Intended for unit tests and
    // for callers that deliberately want the no-vsync fallback.
    //
    // Exception — Emscripten: the timer loop is thread-backed and a wasm build
    // without pthreads cannot construct a std::thread at all, so this returns
    // the requestAnimationFrame loop there. Callers must not assume
    // backend() == timer; check it if the distinction matters.
    static std::unique_ptr<RenderLoop> create_timer_loop();

    virtual ~RenderLoop() = default;

    // Start the render loop with a frame callback. The callback is invoked
    // at (approximately) display refresh rate, but only on frames where a
    // request_frame() has been posted since the last callback — the loop is
    // demand-driven, not free-running, so an idle UI costs nothing.
    virtual void start(FrameCallback on_frame) = 0;

    // Stop the render loop. Idempotent; safe to call before start().
    virtual void stop() = 0;

    // Request a frame be rendered. Marks the loop dirty; the callback fires
    // once on the next vsync. Any number of calls between two vsyncs
    // coalesce into a single callback invocation. RT-safe (lock-free).
    virtual void request_frame() = 0;

    // Is the loop currently running?
    virtual bool is_running() const = 0;

    // Which backend this instance is. Lets callers detect a timer fallback.
    virtual RenderLoopBackend backend() const = 0;
};

} // namespace pulp::render
