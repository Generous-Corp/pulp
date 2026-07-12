// RenderLoop for the browser — requestAnimationFrame.
//
// rAF is the browser's compositor vblank signal, so this is a real vsync
// source, not a timer fallback. It also already runs on the JS main thread,
// which is where the render callback must execute — hence no equivalent of the
// macOS dispatch_async hop back to the main queue.
//
// The dirty-flag coalescing is the shared RenderLoopState: any number of
// request_frame() calls between two animation frames fire the callback exactly
// once, and a stop() clears a pending request so no callback lands after it.
//
// Lifetime invariant: a registered rAF callback cannot be cancelled from C++ —
// it only retires by returning EM_FALSE on its next invocation, which may be
// after the RenderLoop is destroyed. So the callback never dereferences the
// RenderLoop; it owns a strong reference to a heap `RafCore` (state +
// callback) that outlives the loop, and frees that reference when it retires.
// A generation counter, bumped on every stop(), retires registrations from a
// previous start()/stop() cycle so a restart never ends up with two live rAF
// registrations driving one core.

#include <pulp/render/render_loop.hpp>

#if defined(__EMSCRIPTEN__)

#include "render_loop_state.hpp"

#include <emscripten/html5.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace pulp::render {

namespace {

struct RafCore {
    RenderLoopState state;
    FrameCallback callback;
    std::atomic<std::uint64_t> generation{0};
};

// The rAF userData: a strong ref to the core plus the generation it was
// registered for. Heap-allocated at start(), deleted when the callback retires.
struct RafRegistration {
    std::shared_ptr<RafCore> core;
    std::uint64_t generation = 0;
};

} // namespace

class EmscriptenRenderLoop : public RenderLoop {
public:
    EmscriptenRenderLoop() : core_(std::make_shared<RafCore>()) {}

    ~EmscriptenRenderLoop() override { stop(); }

    void start(FrameCallback on_frame) override {
        if (!core_->state.start()) {
            // Already running: keep the active callback, matching the other
            // backends where a repeated start() only re-arms a frame.
            return;
        }
        core_->callback = std::move(on_frame);

        auto* registration = new RafRegistration{
            core_, core_->generation.load(std::memory_order_acquire)};
        emscripten_request_animation_frame_loop(&EmscriptenRenderLoop::on_raf, registration);
    }

    void stop() override {
        core_->state.stop();
        // Retire any live registration: it unregisters itself on its next
        // invocation once its generation is stale.
        core_->generation.fetch_add(1, std::memory_order_acq_rel);
    }

    void request_frame() override { core_->state.request_frame(); }

    bool is_running() const override { return core_->state.is_running(); }

    RenderLoopBackend backend() const override { return RenderLoopBackend::raf; }

private:
    static EM_BOOL on_raf(double /*time_ms*/, void* ctx) {
        auto* registration = static_cast<RafRegistration*>(ctx);
        RafCore& core = *registration->core;

        if (!core.state.is_running() ||
            core.generation.load(std::memory_order_acquire) != registration->generation) {
            delete registration;  // drops the last core ref if the loop is gone
            return EM_FALSE;
        }

        if (core.state.consume_frame_request() && core.state.is_running()) {
            if (core.callback) core.callback();
        }
        return EM_TRUE;
    }

    std::shared_ptr<RafCore> core_;
};

std::unique_ptr<RenderLoop> make_emscripten_render_loop() {
    return std::make_unique<EmscriptenRenderLoop>();
}

} // namespace pulp::render

#endif // __EMSCRIPTEN__
