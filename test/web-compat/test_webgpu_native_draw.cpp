// iOS-D.3c (#3217) — live native WebGPU buffered-draw coverage.
//
// Drives __gpuQueueDrawBufferedImpl end-to-end against a real Dawn device so
// the auto-pipeline-layout + pipeline.GetBindGroupLayout() path the Three.js
// cube depends on is exercised in a headless test (the [phase13] smoke that
// would otherwise cover it needs V8 + three.iife and was never wired into a
// build target). A draw with a uniform bind group only succeeds if the
// auto-layout bind group matches the shader interface — the exact thing that
// silently failed on the iOS Simulator before the fix.
//
// Skips cleanly when no Dawn device is available (non-GPU runners), so it only
// gates where a real GPU surface can be created.

#include <catch2/catch_test_macros.hpp>

#include <pulp/render/gpu_surface.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <memory>
#include <string>

using namespace pulp::view;
using namespace pulp;

namespace {

std::unique_ptr<pulp::render::GpuSurface> make_offscreen_surface(uint32_t w, uint32_t h) {
    auto surface = pulp::render::GpuSurface::create_dawn();
    if (!surface) return nullptr;
    pulp::render::GpuSurface::Config cfg{};
    cfg.width = w;
    cfg.height = h;
    cfg.native_surface_handle = nullptr;  // offscreen — no swapchain needed
    if (!surface->initialize(cfg)) return nullptr;
    return surface;
}

}  // namespace

TEST_CASE("WebGPU native buffered draw: uniform bind group binds through auto pipeline layout",
          "[webcompat][gpu][native][issue-3217]") {
    auto gpu = make_offscreen_surface(64, 64);
    if (!gpu) {
        SKIP("No Dawn GPU device available on this runner");
    }

    View root;
    root.set_bounds({0, 0, 64, 64});
    root.set_theme(Theme::dark());
    state::StateStore store;
    ScriptEngine engine;
    auto bridge = std::make_unique<WidgetBridge>(engine, root, store, gpu.get());
    bridge->load_script("");

    // Configure a native GPUCanvasContext-backed render target.
    const auto configured = engine.evaluate(
        "(function(){ var r = __gpuCanvasConfigureImpl('cv', 64, 64, 'bgra8unorm', 0, 'opaque');"
        " return String(r && r.configured === true); })()");
    if (std::string(configured.getWithDefault<std::string_view>("")) != "true") {
        SKIP("Native GPUCanvasContext configure unavailable (no Skia/Dawn canvas texture)");
    }

    // A fullscreen triangle whose fragment color comes from a uniform bind
    // group. The draw only returns true if the bind group (built from
    // pipeline.GetBindGroupLayout(0)) matches the shader's @group(0)@binding(0)
    // uniform — i.e. the exact auto-layout path under test.
    const auto drew = engine.evaluate(R"JS(
        (function () {
            function f32(arr) {
                return Array.from(new Uint8Array(new Float32Array(arr).buffer));
            }
            var payload = {
                canvasId: 'cv',
                vertexCode:
                    "@vertex fn main(@location(0) pos : vec2<f32>) -> @builtin(position) vec4<f32> {" +
                    "  return vec4<f32>(pos, 0.0, 1.0);" +
                    "}",
                vertexEntryPoint: 'main',
                fragmentCode:
                    "struct C { color : vec4<f32> };" +
                    "@group(0) @binding(0) var<uniform> u : C;" +
                    "@fragment fn main() -> @location(0) vec4<f32> { return u.color; }",
                fragmentEntryPoint: 'main',
                format: 'bgra8unorm',
                topology: 'triangle-list',
                drawType: 'draw',
                vertexCount: 3,
                instanceCount: 1,
                loadOp: 'clear',
                clearValue: { r: 0, g: 0, b: 0, a: 1 },
                vertexBuffers: [{
                    slot: 0,
                    arrayStride: 8,
                    stepMode: 'vertex',
                    attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x2' }],
                    data: f32([-1, -1, 3, -1, -1, 3])
                }],
                bindGroups: [{
                    index: 0,
                    entries: [{
                        binding: 0,
                        resourceType: 'buffer',
                        bufferType: 'uniform',
                        visibility: (GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT),
                        size: 16,
                        data: f32([0, 1, 0, 1])
                    }]
                }]
            };
            return String(__gpuQueueDrawBufferedImpl(payload) === true);
        })()
    )JS");

    INFO("buffered draw result => " << std::string(drew.getWithDefault<std::string_view>("")));
    REQUIRE(std::string(drew.getWithDefault<std::string_view>("")) == "true");
}
