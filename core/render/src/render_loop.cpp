// RenderLoop platform implementations
// Each platform uses its native vsync mechanism for frame-paced rendering.

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

namespace pulp::render {

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

// ── Generic: Timer-based fallback (Windows, Linux, or no-vsync) ─────────

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

private:
    std::thread thread_;
    FrameCallback callback_;
    RenderLoopState state_;
};

// ── Factory ─────────────────────────────────────────────────────────────

std::unique_ptr<RenderLoop> RenderLoop::create(void* native_handle) {
    (void)native_handle;
#if defined(__APPLE__) && !defined(PULP_RENDER_LOOP_FORCE_TIMER) && TARGET_OS_OSX
    return std::make_unique<MacRenderLoop>();
#else
    return std::make_unique<TimerRenderLoop>();
#endif
}

} // namespace pulp::render
