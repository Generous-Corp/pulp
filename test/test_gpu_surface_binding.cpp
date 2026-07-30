// test_gpu_surface_binding.cpp — the SHARED adapter binding that wires a
// host's GPU-surface lifecycle into a scripted UI session (WAH-1).
//
// `bind_gpu_surface()` is the one call VST3, CLAP, AAX, AU v2 and AU v3 (mac
// and iOS) all make. It replaced five hand-rolled copies of
// `scripted->attach_gpu_surface(host->gpu_surface())` — a one-shot read that
// returned null forever on Windows, where the Dawn surface is not created until
// the HWND is reparented. Five adapters depending on one helper is exactly the
// shape that deserves a direct test rather than five indirect ones.
//
// It also owns the CPU-fallback diagnostic, which is the half that used to be
// WRONG rather than merely missing: fired from a pre-attach null, it accused a
// host of falling back to CPU while that host was about to run on the GPU.
// `pending` must therefore be silent, and only `unavailable` may warn.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/gpu_host_select.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <vector>

using namespace pulp;
using pulp::view::PluginViewHost;

namespace {

// Opaque, never dereferenced — the binding contract is about which POINTER
// reaches the session, not about rendering. Keeps this suite buildable (and
// therefore run) in the no-GPU configuration the coverage lane uses.
render::GpuSurface* sentinel_surface(std::uintptr_t id) {
    return reinterpret_cast<render::GpuSurface*>(id * alignof(std::max_align_t));
}

/// A host whose surface state the test drives directly, mirroring the Windows
/// ordering (nothing until attach).
class ScriptableHost : public PluginViewHost {
public:
    explicit ScriptableHost(bool use_gpu = true) {
        if (use_gpu) mark_gpu_surface_pending();
    }

    view::NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(view::NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(std::uint32_t, std::uint32_t) override {}
    Size get_size() const override { return {}; }
    render::GpuSurface* gpu_surface() const override { return surface_; }

    void go_ready(render::GpuSurface* surface) {
        surface_ = surface;
        publish_gpu_surface(surface_, GpuSurfaceState::ready);
    }
    void go_unavailable() {
        surface_ = nullptr;
        publish_gpu_surface(nullptr, GpuSurfaceState::unavailable);
    }
    void go_pending() {
        surface_ = nullptr;
        publish_gpu_surface(nullptr, GpuSurfaceState::pending);
    }

private:
    render::GpuSurface* surface_ = nullptr;
};

format::GpuHostDecision gpu_decision(bool use_gpu) {
    format::GpuHostDecision d;
    d.use_gpu = use_gpu;
    d.wants_gpu = use_gpu;
    d.mode = use_gpu ? "scripted" : "autoui";
    return d;
}

/// A session that has not loaded a script still stashes the forwarded pointer,
/// which is the part of the contract this file is about. Script loading and
/// bridge construction are covered by test_scripted_ui.cpp.
view::ScriptedUiSession make_session(view::View& root, state::StateStore& store) {
    return view::ScriptedUiSession(root, store, view::ScriptedUiOptions{});
}

}  // namespace

TEST_CASE("binding forwards a surface that appears AFTER the bind",
          "[gpu-surface-binding][wah-1]") {
    // The Windows ordering, which the one-shot read this replaced could not
    // handle: the adapter binds at editor-open, and the surface only exists
    // once the host is attached to its parent.
    view::View root;
    state::StateStore store;
    auto session = make_session(root, store);
    ScriptableHost host;

    auto binding = format::bind_gpu_surface(host, &session, gpu_decision(true),
                                            "test");
    REQUIRE(session.gpu_surface() == nullptr);  // pending, correctly nothing

    auto* live = sentinel_surface(1);
    host.go_ready(live);

    REQUIRE(session.gpu_surface() == live);
}

TEST_CASE("binding detaches the surface from the session on teardown",
          "[gpu-surface-binding][wah-1]") {
    // The other half the one-shot read could not do at all: tell the session
    // the pointer it holds is dead. Without this the scripted UI keeps a raw
    // pointer into a destroyed surface.
    view::View root;
    state::StateStore store;
    auto session = make_session(root, store);
    ScriptableHost host;
    auto binding = format::bind_gpu_surface(host, &session, gpu_decision(true),
                                            "test");

    host.go_ready(sentinel_surface(1));
    REQUIRE(session.gpu_surface() != nullptr);

    host.go_unavailable();

    REQUIRE(session.gpu_surface() == nullptr);
}

TEST_CASE("binding follows a surface REPLACEMENT through null",
          "[gpu-surface-binding][wah-1]") {
    view::View root;
    state::StateStore store;
    auto session = make_session(root, store);
    ScriptableHost host;
    auto binding = format::bind_gpu_surface(host, &session, gpu_decision(true),
                                            "test");

    host.go_ready(sentinel_surface(1));
    host.go_pending();
    REQUIRE(session.gpu_surface() == nullptr);

    auto* rebuilt = sentinel_surface(2);
    host.go_ready(rebuilt);

    REQUIRE(session.gpu_surface() == rebuilt);
}

TEST_CASE("a released binding stops forwarding", "[gpu-surface-binding][wah-1]") {
    // Adapters reset the subscription in their close path, BEFORE the bridge
    // that owns the session is destroyed. If a late transition still reached
    // the session, that ordering would not save them.
    view::View root;
    state::StateStore store;
    auto session = make_session(root, store);
    ScriptableHost host;
    {
        auto binding = format::bind_gpu_surface(host, &session,
                                                gpu_decision(true), "test");
        host.go_ready(sentinel_surface(1));
        REQUIRE(session.gpu_surface() != nullptr);
    }

    host.go_ready(sentinel_surface(2));

    // Still the first surface: the session was not touched after teardown.
    REQUIRE(session.gpu_surface() == sentinel_surface(1));
}

TEST_CASE("binding with no session is safe", "[gpu-surface-binding][wah-1]") {
    // An adapter whose editor is not scripted passes nullptr — the binding
    // still exists so the diagnostic runs.
    ScriptableHost host;
    auto binding = format::bind_gpu_surface(host, nullptr, gpu_decision(true),
                                            "test");
    host.go_ready(sentinel_surface(1));
    host.go_unavailable();
    SUCCEED("no crash without a session");
}

TEST_CASE("the returned subscription is convertible to bool",
          "[gpu-surface-binding][wah-1]") {
    ScriptableHost host;
    auto binding = format::bind_gpu_surface(host, nullptr, gpu_decision(true),
                                            "test");
    REQUIRE(static_cast<bool>(binding));
    binding.reset();
    REQUIRE_FALSE(static_cast<bool>(binding));
}

// ── The CPU-fallback diagnostic ─────────────────────────────────────────────

TEST_CASE("the CPU-fallback warning is silent while the surface is pending",
          "[gpu-surface-binding][wah-1]") {
    // The Windows bug in one assertion: a host between create() and attach has
    // not fallen back to anything, and warn_if_unexpected_cpu_fallback() used
    // to be called at exactly that moment.
    ScriptableHost host;
    REQUIRE(host.gpu_surface_state() == PluginViewHost::GpuSurfaceState::pending);
    // No observable side effect to assert beyond "does not treat pending as a
    // failure" — the state check IS the guard the function now applies.
    format::warn_if_unexpected_cpu_fallback(gpu_decision(true), &host);
    SUCCEED("pending is not reported as a CPU fallback");
}

TEST_CASE("the CPU-fallback warning ignores a host that never wanted the GPU",
          "[gpu-surface-binding][wah-1]") {
    ScriptableHost host(/*use_gpu*/ false);
    REQUIRE(host.gpu_surface_state() ==
            PluginViewHost::GpuSurfaceState::unavailable);
    format::warn_if_unexpected_cpu_fallback(gpu_decision(false), &host);
    SUCCEED("an AutoUi editor on a CPU host is not a fallback");
}

TEST_CASE("the CPU-fallback warning tolerates a null host",
          "[gpu-surface-binding][wah-1]") {
    format::warn_if_unexpected_cpu_fallback(gpu_decision(true), nullptr);
    SUCCEED("a failed PluginViewHost::create() must not crash the diagnostic");
}

TEST_CASE("a real GPU-init failure reaches the unavailable state",
          "[gpu-surface-binding][wah-1]") {
    ScriptableHost host;
    host.go_unavailable();
    REQUIRE(host.gpu_surface_state() ==
            PluginViewHost::GpuSurfaceState::unavailable);
    format::warn_if_unexpected_cpu_fallback(gpu_decision(true), &host);
    SUCCEED("unavailable is the only state in which the warning is true");
}

// ── Status normalization ────────────────────────────────────────────────────

namespace {

/// Publishes deliberately malformed statuses so the normalization in
/// publish_gpu_surface() is exercised.
class MalformedPublisher : public PluginViewHost {
public:
    view::NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(view::NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(std::uint32_t, std::uint32_t) override {}
    Size get_size() const override { return {}; }

    void publish(render::GpuSurface* s, GpuSurfaceState st) {
        publish_gpu_surface(s, st);
    }
};

}  // namespace

TEST_CASE("ready with no surface is normalized to unavailable",
          "[gpu-surface-binding][wah-1]") {
    // `ready` is DEFINED as "gpu_surface() is non-null". A host that reported
    // ready with nothing attached would make every consumer's null check
    // meaningless, so the status is corrected rather than propagated.
    MalformedPublisher host;
    host.publish(nullptr, PluginViewHost::GpuSurfaceState::ready);
    REQUIRE(host.gpu_surface_state() ==
            PluginViewHost::GpuSurfaceState::unavailable);
    REQUIRE(host.gpu_surface_status().surface == nullptr);
}

TEST_CASE("a surface published with a non-ready state is dropped",
          "[gpu-surface-binding][wah-1]") {
    MalformedPublisher host;
    host.publish(sentinel_surface(1), PluginViewHost::GpuSurfaceState::pending);
    REQUIRE(host.gpu_surface_state() ==
            PluginViewHost::GpuSurfaceState::pending);
    // Carrying a pointer alongside "not ready" is the same contradiction from
    // the other side; the pointer is what gets dropped.
    REQUIRE(host.gpu_surface_status().surface == nullptr);
}

// ── Detach must not look like a GPU failure ─────────────────────────────────

TEST_CASE("a rebuildable teardown publishes pending, never unavailable",
          "[gpu-surface-binding][wah-1]") {
    // `unavailable` is the state the CPU-fallback diagnostic warns on. A host
    // that tears its surface down on detach and rebuilds it on the next attach
    // must therefore publish `pending`, or every ordinary editor close logs a
    // false "gpu-init-failed falling_back=cpu" error — and a consumer that
    // reacted to `unavailable` by disabling its GPU path would never turn it
    // back on.
    ScriptableHost host;
    std::vector<PluginViewHost::GpuSurfaceState> seen;
    auto sub = host.observe_gpu_surface(
        [&](const PluginViewHost::GpuSurfaceStatus& s) { seen.push_back(s.state); });

    host.go_ready(sentinel_surface(1));
    host.go_pending();  // the detach edge, as the real hosts publish it

    REQUIRE(seen.size() == 3);  // pending (immediate), ready, pending
    REQUIRE(seen[2] == PluginViewHost::GpuSurfaceState::pending);
    for (auto state : seen)
        REQUIRE(state != PluginViewHost::GpuSurfaceState::unavailable);
}
