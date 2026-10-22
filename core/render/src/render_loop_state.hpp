#pragma once

#include <atomic>

#include "pulp/render/render_loop.hpp"

namespace pulp::render {

// Tracks whether a compositor-vblank wait (Windows DwmFlush) has degraded to
// the timer fallback so backend() reports the effective backend.
//
// The Windows loop waits on DwmFlush() for vblank pacing; when DWM is
// unavailable (Server Core, remote session, headless) DwmFlush() fails and
// the loop transparently sleeps on a ~60 Hz timer instead. Without this
// tracker, backend() would keep claiming dwm_flush (a real vblank) while the
// loop is actually on the timer — defeating the very introspection API that
// render_loop_backend_is_vsync() callers rely on to detect a non-vsync path.
//
// The latch is sticky: once DwmFlush() fails we report timer for the rest of
// the loop's life. DWM availability does not flip back mid-session in the
// failure cases this guards (DWM disabled / no compositor), and a sticky flag
// is race-free and conservative — a caller that has seen any vblank miss
// should not be told it is back on a reliable vblank.
class DwmBackendTracker {
public:
    // Record the outcome of one DwmFlush() wait (true == S_OK / real vblank).
    void note_wait_result(bool ok) {
        if (!ok) {
            on_timer_fallback_.store(true, std::memory_order_relaxed);
        }
    }

    // The effective backend given everything observed so far.
    RenderLoopBackend effective_backend() const {
        return on_timer_fallback_.load(std::memory_order_relaxed)
                   ? RenderLoopBackend::timer
                   : RenderLoopBackend::dwm_flush;
    }

private:
    std::atomic<bool> on_timer_fallback_{false};
};

class RenderLoopState {
public:
    bool start() {
        const bool was_running = running_.exchange(true, std::memory_order_acq_rel);
        needs_frame_.store(true, std::memory_order_release);
        return !was_running;
    }

    bool stop() {
        const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
        needs_frame_.store(false, std::memory_order_release);
        return was_running;
    }

    void request_frame() {
        if (is_running()) {
            needs_frame_.store(true, std::memory_order_release);
        }
    }

    bool consume_frame_request() {
        return is_running() && needs_frame_.exchange(false, std::memory_order_acq_rel);
    }

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> needs_frame_{false};
};

} // namespace pulp::render
