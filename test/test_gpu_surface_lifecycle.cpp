// test_gpu_surface_lifecycle.cpp — PluginViewHost's GPU-surface lifecycle
// contract (see plugin_view_host.hpp, "GPU-surface lifecycle").
//
// The bug this pins: every format adapter read `host->gpu_surface()` ONCE, at
// editor-open time, and handed the result to the scripted UI session. That read
// is only correct on hosts that build their surface in the constructor. The
// Windows host cannot — Dawn configures presentation for the HWND's
// native-window shape, and the editor HWND is a hidden WS_POPUP until
// attach_to_parent() reparents it into the DAW — so the read returned null,
// never became non-null, and `navigator.gpu` fell through to mocks on every
// Windows editor. The same one-shot read had nowhere to learn about DETACH, so
// a consumer kept a raw pointer to a destroyed surface.
//
// These tests use a fake host that reproduces the Windows ordering exactly
// (create → attach → detach → reattach → destroy) so the contract is exercised
// on macOS, which is Pulp's required CI gate. A `_WIN32`-gated test of this
// would never run.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/plugin_view_host.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using namespace pulp::view;

namespace {

// The lifecycle contract is about the POINTER's publication and ordering, not
// about rendering: nothing here ever calls through a GpuSurface. So the fake
// host hands out opaque sentinel addresses rather than deriving from
// render::GpuSurface. That keeps this suite buildable — and therefore RUN — in
// configurations without Skia/Dawn, which is where the shared lifecycle logic
// most needs a gate. `render::GpuSurface` stays an incomplete type here, which
// is exactly how PluginViewHost declares it.
using SurfaceHandle = pulp::render::GpuSurface*;

SurfaceHandle make_sentinel_surface(std::uintptr_t id) {
    // Never dereferenced. Distinct non-null values so "did the consumer get a
    // NEW surface?" is answerable.
    return reinterpret_cast<SurfaceHandle>(id * alignof(std::max_align_t));
}

// Mirrors the WINDOWS host's ordering: no surface until attach, torn down on
// detach, rebuilt on reattach.
class DeferredSurfaceHost : public PluginViewHost {
public:
    explicit DeferredSurfaceHost(bool use_gpu = true) {
        if (use_gpu) mark_gpu_surface_pending();
    }
    ~DeferredSurfaceHost() override { release(); }

    NativeViewHandle native_handle() override { return nullptr; }

    void attach_to_parent(NativeViewHandle) override {
        attached_ = true;
        if (creation_fails_) {
            publish_gpu_surface(nullptr, GpuSurfaceState::unavailable);
            return;
        }
        surface_ = make_sentinel_surface(++surface_generation_);
        publish_gpu_surface(surface_, GpuSurfaceState::ready);
    }

    bool is_attached() const noexcept override { return attached_; }

    void detach() override {
        if (!attached_) return;  // idempotent, like the real hosts
        attached_ = false;
        release();
        // The next attach rebuilds — "not yet", not "never".
        publish_gpu_surface(nullptr, GpuSurfaceState::pending);
    }

    void repaint() override {}
    void set_size(uint32_t, uint32_t) override {}
    Size get_size() const override { return {}; }
    pulp::render::GpuSurface* gpu_surface() const override { return surface_; }

    /// Simulate a mid-session drawable loss: tear down and rebuild in place.
    void simulate_surface_recreate() {
        release();
        publish_gpu_surface(nullptr, GpuSurfaceState::pending);
        surface_ = make_sentinel_surface(++surface_generation_);
        publish_gpu_surface(surface_, GpuSurfaceState::ready);
    }

    void set_creation_fails(bool fails) { creation_fails_ = fails; }

    /// Re-publish the CURRENT status verbatim. Real hosts do this defensively
    /// (e.g. a resize path that republishes the same live surface); consumers
    /// must not see churn from it.
    void republish_current() {
        publish_gpu_surface(gpu_surface_status().surface,
                            gpu_surface_status().state);
    }

private:
    void release() {
        publish_gpu_surface(nullptr, GpuSurfaceState::unavailable);
        surface_ = nullptr;
    }

    SurfaceHandle surface_ = nullptr;
    std::uintptr_t surface_generation_ = 0;
    bool attached_ = false;
    bool creation_fails_ = false;
};

/// Records every status an observer is handed, so ORDER can be asserted — the
/// property that matters most here (a consumer must never see `ready` after the
/// surface is gone, and must never miss the transition to null).
struct Recorder {
    std::vector<PluginViewHost::GpuSurfaceStatus> seen;

    PluginViewHost::GpuSurfaceObserver observer() {
        return [this](const PluginViewHost::GpuSurfaceStatus& s) {
            seen.push_back(s);
        };
    }
    PluginViewHost::GpuSurfaceState state_at(size_t i) const {
        return seen.at(i).state;
    }
    pulp::render::GpuSurface* surface_at(size_t i) const {
        return seen.at(i).surface;
    }
};

}  // namespace

TEST_CASE("pre-attach null reads as pending, not unavailable",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;

    // This is the exact state every format adapter samples when it opens the
    // editor. It used to be indistinguishable from a CPU host, which is why the
    // Windows editor logged `gpu-init-failed falling_back=cpu` while running on
    // the GPU.
    REQUIRE(host.gpu_surface() == nullptr);
    REQUIRE(host.gpu_surface_state() == PluginViewHost::GpuSurfaceState::pending);
}

TEST_CASE("a CPU host reports unavailable, never pending",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host(/*use_gpu*/ false);
    REQUIRE(host.gpu_surface_state() ==
            PluginViewHost::GpuSurfaceState::unavailable);
}

TEST_CASE("observer sees the current status immediately on subscribe",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;

    auto sub = host.observe_gpu_surface(rec.observer());

    // Immediate delivery is what removes "did I subscribe too late?" from every
    // consumer: a late subscriber gets the current state, not silence.
    REQUIRE(rec.seen.size() == 1);
    REQUIRE(rec.state_at(0) == PluginViewHost::GpuSurfaceState::pending);
    REQUIRE(rec.surface_at(0) == nullptr);
    REQUIRE(sub.active());
}

TEST_CASE("delayed creation is published to an already-subscribed consumer",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());

    host.attach_to_parent(nullptr);

    REQUIRE(rec.seen.size() == 2);
    REQUIRE(rec.state_at(1) == PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(rec.surface_at(1) == host.gpu_surface());
    REQUIRE(rec.surface_at(1) != nullptr);
}

TEST_CASE("a consumer subscribing after creation still gets the live surface",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    host.attach_to_parent(nullptr);

    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());

    REQUIRE(rec.seen.size() == 1);
    REQUIRE(rec.state_at(0) == PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(rec.surface_at(0) == host.gpu_surface());
}

TEST_CASE("detach publishes null before the surface dies, then pending",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());
    host.attach_to_parent(nullptr);
    const auto* live = host.gpu_surface();
    REQUIRE(live != nullptr);

    host.detach();

    // pending → ready → unavailable → pending. The `unavailable` edge is the
    // one a polling consumer could never see: it is what tells the scripted UI
    // to drop the raw pointer while it is still safe to do so.
    REQUIRE(rec.seen.size() == 4);
    REQUIRE(rec.state_at(2) == PluginViewHost::GpuSurfaceState::unavailable);
    REQUIRE(rec.surface_at(2) == nullptr);
    REQUIRE(rec.state_at(3) == PluginViewHost::GpuSurfaceState::pending);
    REQUIRE(host.gpu_surface() == nullptr);
}

TEST_CASE("reattach publishes a fresh surface",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());

    host.attach_to_parent(nullptr);
    auto* first = host.gpu_surface();
    host.detach();
    host.attach_to_parent(nullptr);
    auto* second = host.gpu_surface();

    REQUIRE(second != nullptr);
    REQUIRE(second != first);  // a genuinely new surface, not the stale one
    REQUIRE(rec.seen.back().state == PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(rec.seen.back().surface == second);
}

TEST_CASE("surface recreation mid-session round-trips through null",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    host.attach_to_parent(nullptr);

    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());
    host.simulate_surface_recreate();

    // ready (immediate) → unavailable → pending → ready. A consumer must never
    // go straight from one live pointer to another: it has to be told the first
    // one died, or it will keep using a freed surface for one transition.
    REQUIRE(rec.seen.size() == 4);
    REQUIRE(rec.state_at(0) == PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(rec.state_at(1) == PluginViewHost::GpuSurfaceState::unavailable);
    REQUIRE(rec.state_at(2) == PluginViewHost::GpuSurfaceState::pending);
    REQUIRE(rec.state_at(3) == PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(rec.surface_at(3) == host.gpu_surface());
}

TEST_CASE("failed creation publishes unavailable, so the CPU warning is true",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    host.set_creation_fails(true);
    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());

    host.attach_to_parent(nullptr);

    REQUIRE(rec.seen.size() == 2);
    REQUIRE(rec.state_at(1) == PluginViewHost::GpuSurfaceState::unavailable);
    REQUIRE(host.gpu_surface_state() ==
            PluginViewHost::GpuSurfaceState::unavailable);
}

TEST_CASE("host destruction notifies surviving subscribers",
          "[gpu-surface-lifecycle][wah-1]") {
    Recorder rec;
    PluginViewHost::GpuSurfaceSubscription sub;
    {
        DeferredSurfaceHost host;
        sub = host.observe_gpu_surface(rec.observer());
        host.attach_to_parent(nullptr);
        REQUIRE(rec.seen.back().state == PluginViewHost::GpuSurfaceState::ready);
    }

    // The host is gone. A consumer that outlives it must have been told, or it
    // is holding a pointer into freed memory with no way to find out.
    REQUIRE(rec.seen.back().state == PluginViewHost::GpuSurfaceState::unavailable);
    REQUIRE(rec.seen.back().surface == nullptr);
    // And the subscription is inert rather than dangling.
    REQUIRE_FALSE(sub.active());
    sub.reset();  // must not crash
}

TEST_CASE("no callback fires after the consumer unsubscribes",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());
    const size_t after_subscribe = rec.seen.size();

    sub.reset();
    host.attach_to_parent(nullptr);
    host.detach();

    REQUIRE(rec.seen.size() == after_subscribe);
}

TEST_CASE("a subscription destroyed by scope exit stops delivery",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;
    {
        auto sub = host.observe_gpu_surface(rec.observer());
        host.attach_to_parent(nullptr);
    }
    const size_t before = rec.seen.size();

    host.detach();

    REQUIRE(rec.seen.size() == before);
}

TEST_CASE("moving a subscription transfers ownership exactly once",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;

    auto original = host.observe_gpu_surface(rec.observer());
    auto moved = std::move(original);
    REQUIRE_FALSE(original.active());
    REQUIRE(moved.active());

    const size_t before = rec.seen.size();
    host.attach_to_parent(nullptr);
    REQUIRE(rec.seen.size() == before + 1);  // delivered once, not twice

    moved.reset();
    host.detach();
    REQUIRE(rec.seen.size() == before + 1);  // and not at all after reset
}

TEST_CASE("multiple observers all see the same transitions",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder a, b;
    auto sub_a = host.observe_gpu_surface(a.observer());
    auto sub_b = host.observe_gpu_surface(b.observer());

    host.attach_to_parent(nullptr);

    REQUIRE(a.seen.back().state == PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(b.seen.back().state == PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(a.seen.back().surface == b.seen.back().surface);

    // Dropping one must not disturb the other.
    sub_a.reset();
    const size_t a_before = a.seen.size();
    host.detach();
    REQUIRE(a.seen.size() == a_before);
    REQUIRE(b.seen.back().state == PluginViewHost::GpuSurfaceState::pending);
}

TEST_CASE("an observer that unsubscribes from inside its own callback is safe",
          "[gpu-surface-lifecycle][wah-1]") {
    // A scripted-UI teardown reacting to `unavailable` does exactly this.
    DeferredSurfaceHost host;
    auto sub = std::make_shared<PluginViewHost::GpuSurfaceSubscription>();
    int calls = 0;

    *sub = host.observe_gpu_surface(
        [sub, &calls](const PluginViewHost::GpuSurfaceStatus& s) {
            ++calls;
            if (s.state == PluginViewHost::GpuSurfaceState::ready) sub->reset();
        });
    REQUIRE(calls == 1);  // pending, on subscribe

    host.attach_to_parent(nullptr);
    REQUIRE(calls == 2);  // ready — and unsubscribed from inside

    host.detach();
    REQUIRE(calls == 2);  // nothing after the self-unsubscribe
}

TEST_CASE("publishing an identical status does not re-notify",
          "[gpu-surface-lifecycle][wah-1]") {
    DeferredSurfaceHost host;
    Recorder rec;
    auto sub = host.observe_gpu_surface(rec.observer());
    host.attach_to_parent(nullptr);
    const size_t before = rec.seen.size();

    // Hosts publish defensively (a resize path re-announces the same live
    // surface). A consumer must not see churn from that, or a scripted UI would
    // re-attach its GPU bridge on every frame.
    host.republish_current();
    host.republish_current();

    REQUIRE(rec.seen.size() == before);
}

TEST_CASE("a host with no observers still tracks its own state",
          "[gpu-surface-lifecycle][wah-1]") {
    // The registry is allocated lazily; a host nobody watches must still answer
    // gpu_surface_state() correctly, because warn_if_unexpected_cpu_fallback()
    // reads exactly that.
    DeferredSurfaceHost host;
    REQUIRE(host.gpu_surface_state() == PluginViewHost::GpuSurfaceState::pending);
    host.attach_to_parent(nullptr);
    REQUIRE(host.gpu_surface_state() == PluginViewHost::GpuSurfaceState::ready);
    host.detach();
    REQUIRE(host.gpu_surface_state() == PluginViewHost::GpuSurfaceState::pending);
}
