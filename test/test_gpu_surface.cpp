#include <catch2/catch_test_macros.hpp>
#include <pulp/render/gpu_surface.hpp>

#include <cstdlib>
#include <string_view>

using namespace pulp::render;

namespace {

// An unreachable GPU must SKIP, out loud. Catch2 records a bare early `return`
// as a PASS, so a runner with no adapter would report this whole file green
// having asserted nothing about GpuSurface at all.
constexpr const char* kNoGpu =
    "no GPU compute device available (Skia/Dawn not built, or no adapter)";

constexpr const char* kNoSurfaceInit =
    "GpuSurface would not initialize (no GPU adapter available)";

}  // namespace

// Every other case here treats a missing adapter as a reason to SKIP, which is
// right for a developer laptop and for a hosted runner with no representative
// GPU. The cost is that no arrangement of results distinguishes "the GPU lane
// ran" from "the adapter vanished and every GPU case skipped".
// PULP_REQUIRE_GPU_ADAPTER is a lane's promise, not a device probe: where it is
// set, an absent adapter is a failure rather than a skip. Unset, this case
// skips, so it stays inert on every lane that promises nothing.
TEST_CASE("A lane that requires a GPU adapter has one", "[render][gpu][adapter-contract]") {
    const char* promised = std::getenv("PULP_REQUIRE_GPU_ADAPTER");
    const std::string_view value = promised != nullptr ? promised : "";
    if (value.empty() || value == "0") {
        SKIP("PULP_REQUIRE_GPU_ADAPTER is unset: this lane promises no GPU adapter");
    }

    auto surface = GpuSurface::create_dawn();
    REQUIRE(surface != nullptr);

    GpuSurface::Config config{};
    config.width = 256;
    config.height = 256;
    REQUIRE(surface->initialize(config));
    REQUIRE(surface->is_initialized());

    const auto info = surface->adapter_info();
    INFO("adapter: " << info.name << " / " << info.backend << " (" << info.backend_type << ")");
    REQUIRE(info.available);
    // Dawn's Null backend validates API calls and renders nothing, so a surface
    // on it satisfies every check above while compositing an empty frame.
    REQUIRE_FALSE(info.null_backend);
    REQUIRE(info.adapter_type != GpuSurface::AdapterType::cpu);
}

TEST_CASE("GpuSurface factory returns non-null when WebGPU available", "[render][gpu]") {
    auto surface = GpuSurface::create_dawn();
#if defined(PULP_HAS_WEBGPU) || defined(PULP_HAS_SKIA)
    REQUIRE(surface != nullptr);
#else
    REQUIRE(surface == nullptr);
#endif
}

TEST_CASE("GpuSurface offscreen initialization", "[render][gpu]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    GpuSurface::Config config;
    config.width = 800;
    config.height = 600;
    config.native_surface_handle = nullptr;  // offscreen mode

    bool ok = surface->initialize(config);
    if (!ok) {
        // No GPU adapter (headless CI) — verify graceful failure
        REQUIRE_FALSE(surface->is_initialized());
        return;
    }

    REQUIRE(surface->is_initialized());
    REQUIRE(surface->width() == 800);
    REQUIRE(surface->height() == 600);
    REQUIRE_FALSE(surface->has_surface());  // no native layer = no presentable surface
}

TEST_CASE("GpuSurface offscreen frame cycle", "[render][gpu]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    GpuSurface::Config config{};
    config.width = 400;
    config.height = 300;

    if (!surface->initialize(config)) SKIP(kNoSurfaceInit);

    // Offscreen: begin_frame always succeeds, end_frame just processes events
    REQUIRE(surface->begin_frame());
    surface->end_frame();

    // Multiple frames
    for (int i = 0; i < 3; ++i) {
        REQUIRE(surface->begin_frame());
        surface->end_frame();
    }
}

TEST_CASE("GpuSurface resize updates dimensions", "[render][gpu]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    GpuSurface::Config config{};
    config.width = 400;
    config.height = 300;

    if (!surface->initialize(config)) SKIP(kNoSurfaceInit);

    surface->resize(1024, 768);
    REQUIRE(surface->width() == 1024);
    REQUIRE(surface->height() == 768);
}

TEST_CASE("GpuSurface resize before initialize records pending dimensions", "[render][gpu][issue-646]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    REQUIRE_FALSE(surface->is_initialized());

    surface->resize(321, 123);

    REQUIRE(surface->width() == 321);
    REQUIRE(surface->height() == 123);
    REQUIRE_FALSE(surface->is_initialized());
    REQUIRE_FALSE(surface->has_surface());
    REQUIRE_FALSE(surface->begin_frame());
}

TEST_CASE("GpuSurface exposes Dawn handles when initialized", "[render][gpu]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    // Before init: handles should be available but may point to null internals
    GpuSurface::Config config{};
    config.width = 100;
    config.height = 100;

    if (!surface->initialize(config)) SKIP(kNoSurfaceInit);

    // Dawn C++ interop handles only exist on the Skia-backed path.
#ifdef PULP_HAS_SKIA
    REQUIRE(surface->dawn_device_handle() != nullptr);
    REQUIRE(surface->dawn_instance_handle() != nullptr);
    REQUIRE(surface->dawn_queue_handle() != nullptr);

    // No native surface = no current texture
    REQUIRE(surface->current_texture_handle() != nullptr);  // pointer to member, not the texture value
#else
    REQUIRE(surface->dawn_device_handle() == nullptr);
    REQUIRE(surface->dawn_instance_handle() == nullptr);
    REQUIRE(surface->dawn_queue_handle() == nullptr);
    REQUIRE(surface->current_texture_handle() == nullptr);
#endif
}

TEST_CASE("GpuSurface adapter_info reflects initialization state", "[render][gpu][issue-646]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    auto before = surface->adapter_info();
    REQUIRE_FALSE(before.available);
    REQUIRE_FALSE(before.native_bridge);
    REQUIRE_FALSE(before.backend.empty());
    REQUIRE_FALSE(before.backend_type.empty());
    REQUIRE_FALSE(before.name.empty());
    REQUIRE_FALSE(before.vendor.empty());
    REQUIRE_FALSE(before.architecture.empty());
    REQUIRE_FALSE(before.description.empty());
    REQUIRE_FALSE(before.preferred_canvas_format.empty());

    GpuSurface::Config config{};
    config.width = 64;
    config.height = 32;
    config.native_surface_handle = nullptr;

    if (!surface->initialize(config)) {
        auto failed = surface->adapter_info();
        REQUIRE_FALSE(failed.available);
        REQUIRE_FALSE(failed.native_bridge);
        REQUIRE_FALSE(failed.preferred_canvas_format.empty());
        return;
    }

    auto after = surface->adapter_info();
    REQUIRE(after.available);
    REQUIRE(after.native_bridge);
    REQUIRE_FALSE(after.backend.empty());
    REQUIRE_FALSE(after.backend_type.empty());
    REQUIRE_FALSE(after.name.empty());
    REQUIRE_FALSE(after.vendor.empty());
    REQUIRE_FALSE(after.architecture.empty());
    REQUIRE_FALSE(after.description.empty());
    REQUIRE_FALSE(after.preferred_canvas_format.empty());
}

TEST_CASE("GpuSurface begin_frame before initialize returns false", "[render][gpu]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    REQUIRE_FALSE(surface->begin_frame());
}

TEST_CASE("GpuSurface has_surface false without native layer", "[render][gpu]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) SKIP(kNoGpu);

    GpuSurface::Config config{};
    config.native_surface_handle = nullptr;

    if (!surface->initialize(config)) SKIP(kNoSurfaceInit);

    REQUIRE_FALSE(surface->has_surface());
}
