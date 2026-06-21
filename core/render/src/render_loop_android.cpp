// Android RenderLoop — AChoreographer-backed vblank source.
//
// AChoreographer is Android's native vsync callback (API 24+); it pumps
// cleanly on 60/90/120 Hz and VRR displays. The existing standalone
// Choreographer pacer lives in core/render/platform/android/choreographer_android.cpp;
// this is the RenderLoop-shaped wrapper wired into RenderLoop::create().
//
// AChoreographer_getInstance() is thread/looper-sensitive: it must be
// called from a thread with an ALooper attached (the Android UI / render
// thread). RenderLoop::create() is therefore expected to be called from
// that thread on Android. If no Choreographer is available start() stops
// the loop immediately so is_running() reports false (rather than a loop
// that claims to run but can never tick).
//
// LIFETIME — AChoreographer_postFrameCallback cannot be cancelled, so a
// callback posted before stop() can still fire after the AndroidRenderLoop
// object is destroyed. The callback therefore must NOT capture a raw
// `this`. Instead the loop's mutable state lives in a heap ControlBlock
// owned by a std::shared_ptr; each posted callback carries a heap token
// holding a std::weak_ptr to it. When the callback fires it locks the
// weak_ptr — if the loop is gone, lock() fails and the callback is a safe
// no-op. No leak (the token is freed at callback entry), no UAF, and no
// undocumented "must outlive one vsync" contract.
//
// RenderLoopState is the shared platform-agnostic dirty-flag coalescer.

#include <pulp/render/render_loop.hpp>
#include "render_loop_state.hpp"

#if defined(__ANDROID__) && !defined(PULP_RENDER_LOOP_FORCE_TIMER)

#include <android/choreographer.h>
#include <android/looper.h>

#include <memory>

namespace pulp::render {

namespace {

// Heap-owned mutable state, kept alive independently of the RenderLoop
// object so a trailing AChoreographer callback can safely observe it.
struct AndroidLoopControl {
    RenderLoopState state;
    FrameCallback   callback;
    AChoreographer* choreographer = nullptr;
};

// Forward decl — the static vsync callback re-posts itself.
void post_next_frame(const std::shared_ptr<AndroidLoopControl>& control);

// AChoreographer_postFrameCallback uses `long` for the timestamp and a
// `void*` payload. The payload is a heap token holding a weak_ptr to the
// control block; it is deleted as soon as the callback fires.
void on_vsync(long /*frame_time_nanos*/, void* data) {
    std::unique_ptr<std::weak_ptr<AndroidLoopControl>> token(
        static_cast<std::weak_ptr<AndroidLoopControl>*>(data));

    auto control = token->lock();
    if (!control) {
        return;  // RenderLoop was destroyed — safe no-op.
    }
    if (!control->state.is_running()) {
        return;  // Stopped — do not re-post.
    }
    if (control->state.consume_frame_request()) {
        if (control->callback) control->callback();
    }
    if (control->state.is_running()) {
        post_next_frame(control);  // Continuous demand-driven loop.
    }
}

void post_next_frame(const std::shared_ptr<AndroidLoopControl>& control) {
    if (!control->choreographer) {
        return;
    }
    auto* token = new std::weak_ptr<AndroidLoopControl>(control);
    AChoreographer_postFrameCallback(control->choreographer, &on_vsync, token);
}

} // namespace

class AndroidRenderLoop : public RenderLoop {
public:
    AndroidRenderLoop() : control_(std::make_shared<AndroidLoopControl>()) {}
    ~AndroidRenderLoop() override { stop(); }

    void start(FrameCallback on_frame) override {
        if (!control_->state.start()) {
            return;
        }
        control_->callback = std::move(on_frame);

        // AChoreographer_getInstance() requires an ALooper on this thread.
        control_->choreographer = AChoreographer_getInstance();
        if (!control_->choreographer) {
            // No vsync source — report not-running rather than a loop that
            // claims to run but can never tick.
            control_->state.stop();
            return;
        }
        post_next_frame(control_);
    }

    void stop() override {
        control_->state.stop();
        // A callback already posted to AChoreographer cannot be cancelled,
        // but it observes state.is_running()==false (or, after this object
        // is destroyed, a dead weak_ptr) and will not re-post.
        control_->choreographer = nullptr;
    }

    void request_frame() override {
        control_->state.request_frame();
    }

    bool is_running() const override { return control_->state.is_running(); }

    RenderLoopBackend backend() const override {
        return RenderLoopBackend::choreographer;
    }

private:
    std::shared_ptr<AndroidLoopControl> control_;
};

std::unique_ptr<RenderLoop> make_android_render_loop() {
    return std::make_unique<AndroidRenderLoop>();
}

} // namespace pulp::render

#endif // __ANDROID__
