// Typed GPU pass API on a real Dawn device.
//
// Two kinds of device, following the existing GPU test gating:
//   * Pixel cases need a native adapter. They SKIP when Dawn cannot create
//     one or only offers the Null backend, which draws nothing.
//   * Validation cases (WGSL diagnostics, misuse, device loss) run on Dawn's
//     Null adapter when no native one exists, because the Null backend still
//     runs the WGSL compiler and API validation.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/render/gpu_pass.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#if defined(PULP_HAS_SKIA)
#include "webgpu/webgpu_cpp.h"
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

using namespace pulp::render;

namespace {

constexpr const char* kNoNativeGpu = "No native Dawn adapter on this host";
constexpr const char* kNoDawn = "No Dawn device could be created on this host";

// A fullscreen triangle filled with a uniform color.
constexpr const char* kSolidWgsl = R"wgsl(
struct Params {
    color: vec4f,
};
@group(0) @binding(0) var<uniform> params: Params;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    let corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    return vec4f(corners[index], 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
    return params.color;
}
)wgsl";

// Quadrants by framebuffer position: red where x < 25, green where y < 10.
// Proves the top-left origin and per-row readback compaction.
constexpr const char* kQuadrantWgsl = R"wgsl(
@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    let corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    return vec4f(corners[index], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let r = select(0.0, 1.0, position.x < 25.0);
    let g = select(0.0, 1.0, position.y < 10.0);
    return vec4f(r, g, 0.0, 1.0);
}
)wgsl";

struct Rig {
    std::unique_ptr<GpuSurface> surface;
    GpuDevice device;
    bool native = false;
};

// Declared order matters: the device must be released before the surface.
// Rig's members are destroyed in reverse order, device first.
std::unique_ptr<Rig> make_rig(bool allow_null, std::uint32_t width = 64,
                              std::uint32_t height = 32) {
    auto try_backend = [&](GpuSurface::AdapterBackendPreference backend) -> std::unique_ptr<Rig> {
        auto rig = std::make_unique<Rig>();
        rig->surface = GpuSurface::create_dawn();
        if (!rig->surface)
            return nullptr;
        GpuSurface::Config config;
        config.width = width;
        config.height = height;
        config.native_surface_handle = nullptr;
        config.backend_preference = backend;
        if (!rig->surface->initialize(config))
            return nullptr;
        auto device = GpuDevice::from_surface(*rig->surface);
        if (!device)
            return nullptr;
        rig->device = std::move(device.value());
        rig->native = !rig->surface->adapter_info().null_backend;
        return rig;
    };
    // PULP_TEST_GPU_PASS_FORCE_NULL=1 runs the suite as a host without a
    // native adapter sees it: pixel cases skip, validation cases use Null.
    const char* force_null = std::getenv("PULP_TEST_GPU_PASS_FORCE_NULL");
    if (force_null == nullptr || std::string(force_null) != "1") {
        if (auto rig = try_backend(GpuSurface::AdapterBackendPreference::default_backend);
            rig && rig->native)
            return rig;
    }
    if (!allow_null)
        return nullptr;
    return try_backend(GpuSurface::AdapterBackendPreference::null_backend);
}

GpuRenderPipeline make_solid_pipeline(const GpuDevice& device, GpuFormat format) {
    auto shader = device.create_shader_module(kSolidWgsl, "solid.wgsl");
    REQUIRE(shader);
    GpuRenderPipelineDesc desc;
    desc.module = shader.value();
    desc.color_targets = {GpuColorTargetDesc{format, GpuBlend::replace}};
    desc.label = "solid";
    auto pipeline = device.create_render_pipeline(desc);
    if (!pipeline)
        FAIL(pipeline.error().describe());
    return pipeline.value();
}

} // namespace

TEST_CASE("GpuDevice wraps the surface device and reports capabilities",
          "[render][gpu][gpu-pass]") {
    auto rig = make_rig(true);
    if (!rig)
        SKIP(kNoDawn);
    const auto& caps = rig->device.capabilities();
    CHECK(caps.facts.available);
    CHECK(caps.facts.null_backend == !rig->native);
    // WebGPU guarantees these minimums on every conforming device.
    CHECK(caps.facts.max_texture_dimension_2d >= 8192);
    CHECK(caps.facts.max_color_attachments >= 8);
    CHECK(caps.max_bind_groups >= 4);
    CHECK(caps.support(GpuFormat::rgba8_unorm).renderable);
    CHECK(caps.sample_counts(GpuFormat::bgra8_unorm) == std::vector<std::uint32_t>{1, 4});
    if (rig->native)
        CHECK(pick_tier(caps.facts) != GpuRenderTier::none);
    else
        CHECK(pick_tier(caps.facts) == GpuRenderTier::none);
    CHECK_FALSE(rig->device.is_lost());
}

TEST_CASE("A WGSL compile error returns line and column diagnostics",
          "[render][gpu][gpu-pass][wgsl]") {
    auto rig = make_rig(true);
    if (!rig)
        SKIP(kNoDawn);
    constexpr const char* kBroken = "@fragment\n"
                                    "fn fs_main() -> @location(0) vec4f {\n"
                                    "    return colour;\n"
                                    "}\n";
    auto shader = rig->device.create_shader_module(kBroken, "broken.wgsl");
    REQUIRE_FALSE(shader);
    const auto& error = shader.error();
    CHECK(error.kind == GpuError::Kind::shader_compilation);
    REQUIRE_FALSE(error.diagnostics.empty());
    const auto& first = error.diagnostics.front();
    CHECK(first.severity == WgslDiagnostic::Severity::error);
    CHECK(first.line == 3);
    CHECK(first.column == 12);
    CHECK(first.message.find("colour") != std::string::npos);
    INFO(error.describe());
    // The scoped compile kept the error off the device's uncaptured path, so
    // the device is still usable afterwards.
    auto good = rig->device.create_shader_module(kSolidWgsl, "solid.wgsl");
    CHECK(good);
    CHECK_FALSE(rig->device.is_lost());
}

TEST_CASE("Pipeline and descriptor mistakes come back as errors", "[render][gpu][gpu-pass]") {
    auto rig = make_rig(true);
    if (!rig)
        SKIP(kNoDawn);
    auto& device = rig->device;

    auto shader = device.create_shader_module(kSolidWgsl, "solid.wgsl");
    REQUIRE(shader);
    GpuRenderPipelineDesc desc;
    desc.module = shader.value();
    desc.fragment_entry = "missing_entry";
    auto pipeline = device.create_render_pipeline(desc);
    REQUIRE_FALSE(pipeline);
    CHECK(pipeline.error().kind == GpuError::Kind::validation);

    auto texture = device.create_texture({.width = 0, .height = 4});
    REQUIRE_FALSE(texture);
    CHECK(texture.error().kind == GpuError::Kind::invalid_argument);

    auto storage_bgra = device.create_texture({.width = 4,
                                               .height = 4,
                                               .format = GpuFormat::bgra8_unorm,
                                               .usage = GpuTextureUsage::storage});
    if (!device.capabilities().facts.features.bgra8_unorm_storage) {
        REQUIRE_FALSE(storage_bgra);
        CHECK(storage_bgra.error().kind == GpuError::Kind::invalid_argument);
    }

    auto target = device.create_target({.width = 8, .height = 8});
    REQUIRE(target);
    auto solid = make_solid_pipeline(device, GpuFormat::rgba8_unorm);
    // Group 3 does not exist in the pipeline's automatic layout.
    auto buffer = device.create_buffer({.size = 16});
    REQUIRE(buffer);
    auto bad_group = device.create_bind_group(solid, 3, {GpuBinding::of(0, buffer.value())});
    REQUIRE_FALSE(bad_group);
    CHECK(bad_group.error().kind == GpuError::Kind::validation);
}

TEST_CASE("GpuFrame refuses overlapping passes and unsubmitted reads", "[render][gpu][gpu-pass]") {
    auto rig = make_rig(true);
    if (!rig)
        SKIP(kNoDawn);
    auto& device = rig->device;
    auto target = device.create_target({.width = 16, .height = 16});
    REQUIRE(target);

    SECTION("two open passes fail the frame") {
        GpuFrame frame(device, "overlap");
        auto first = frame.render_pass(GpuRenderPassDesc::for_target(target.value()));
        CHECK(first.valid());
        auto second = frame.compute_pass("second");
        CHECK_FALSE(second.valid());
        first.end();
        auto submitted = frame.submit();
        REQUIRE_FALSE(submitted);
        CHECK(submitted.error().kind == GpuError::Kind::invalid_argument);
        auto again = frame.submit();
        REQUIRE_FALSE(again);
        CHECK(again.error().kind == GpuError::Kind::invalid_argument);
    }

    SECTION("a texture with an unsubmitted writer cannot be composited or read") {
        pulp::canvas::RecordingCanvas canvas;
        GpuFrame frame(device, "pending");
        {
            auto pass = frame.render_pass(GpuRenderPassDesc::for_target(target.value()));
            pass.end();
        }
        auto drawn = draw_gpu_texture(canvas, target->color(), 0, 0, 16, 16);
        REQUIRE_FALSE(drawn);
        CHECK(drawn.error().kind == GpuError::Kind::invalid_argument);
        auto early = target->read_pixels();
        REQUIRE_FALSE(early);
        CHECK(early.error().kind == GpuError::Kind::invalid_argument);

        auto submitted = frame.submit();
        REQUIRE(submitted);
        // Submitted: the ordering rule is met, and a non-GPU canvas is the
        // remaining reason to refuse.
        auto after = draw_gpu_texture(canvas, target->color(), 0, 0, 16, 16);
        REQUIRE_FALSE(after);
        CHECK(after.error().kind == GpuError::Kind::unavailable);
    }

    SECTION("destroying an unsubmitted frame submits it") {
        {
            GpuFrame frame(device, "implicit");
            auto pass = frame.render_pass(GpuRenderPassDesc::for_target(target.value()));
        }
        pulp::canvas::RecordingCanvas canvas;
        auto after = draw_gpu_texture(canvas, target->color(), 0, 0, 16, 16);
        REQUIRE_FALSE(after);
        CHECK(after.error().kind == GpuError::Kind::unavailable);
    }
}

TEST_CASE("Fullscreen triangle renders a known pixel into an offscreen target",
          "[render][gpu][gpu-pass][headless]") {
    auto rig = make_rig(false);
    if (!rig)
        SKIP(kNoNativeGpu);
    auto& device = rig->device;

    // 50 px wide: 200 bytes per row, so the readback buffer pads every row.
    auto target = device.create_target({.width = 50, .height = 20, .label = "triangle"});
    REQUIRE(target);
    auto pipeline = make_solid_pipeline(device, GpuFormat::rgba8_unorm);
    auto params = device.create_buffer({.size = 16, .label = "params"});
    REQUIRE(params);
    const std::array<float, 4> orange{1.0f, 0.5f, 0.0f, 1.0f};
    REQUIRE(device.write_buffer(params.value(), 0, orange.data(), sizeof(orange)));
    auto group = device.create_bind_group(pipeline, 0, {GpuBinding::of(0, params.value())});
    REQUIRE(group);

    GpuFrame frame(device, "triangle");
    auto pass = frame.render_pass(GpuRenderPassDesc::for_target(target.value(), {{0, 0, 1, 1}}));
    pass.set_pipeline(pipeline);
    pass.set_bind_group(0, group.value());
    pass.draw(3);
    pass.end();
    auto submission = frame.submit();
    if (!submission)
        FAIL(submission.error().describe());
    CHECK(submission->wait(std::chrono::seconds(5)) == GpuSubmissionStatus::succeeded);

    auto pixels = target->read_pixels();
    if (!pixels)
        FAIL(pixels.error().describe());
    REQUIRE(pixels->width == 50);
    REQUIRE(pixels->bytes.size() == 50u * 20u * 4u);
    for (auto [x, y] : {std::array<std::uint32_t, 2>{0, 0}, {49, 0}, {0, 19}, {49, 19}, {25, 10}}) {
        const auto p = pixels->rgba8_at(x, y);
        INFO("pixel " << x << "," << y);
        CHECK(p[0] == 255);
        CHECK((p[1] >= 127 && p[1] <= 128));
        CHECK(p[2] == 0);
        CHECK(p[3] == 255);
    }
}

TEST_CASE("Position-dependent pass reads back with top-left origin",
          "[render][gpu][gpu-pass][headless]") {
    auto rig = make_rig(false);
    if (!rig)
        SKIP(kNoNativeGpu);
    auto& device = rig->device;
    auto target = device.create_target({.width = 50,
                                        .height = 20,
                                        .color_format = GpuFormat::bgra8_unorm,
                                        .depth_format = GpuFormat::depth24_plus});
    REQUIRE(target);
    REQUIRE(target->depth());
    auto shader = device.create_shader_module(kQuadrantWgsl, "quadrant.wgsl");
    REQUIRE(shader);
    GpuRenderPipelineDesc desc;
    desc.module = shader.value();
    desc.color_targets = {GpuColorTargetDesc{GpuFormat::bgra8_unorm}};
    desc.depth = GpuDepthStateDesc{};
    auto pipeline = device.create_render_pipeline(desc);
    if (!pipeline)
        FAIL(pipeline.error().describe());

    {
        GpuFrame frame(device, "quadrant");
        auto pass = frame.render_pass(GpuRenderPassDesc::for_target(target.value()));
        pass.set_pipeline(pipeline.value());
        pass.draw(3);
        pass.end();
        auto submitted = frame.submit();
        if (!submitted)
            FAIL(submitted.error().describe());
    }
    auto pixels = target->read_pixels();
    if (!pixels)
        FAIL(pixels.error().describe());
    using Rgba = std::array<std::uint8_t, 4>;
    CHECK(pixels->rgba8_at(0, 0) == Rgba{255, 255, 0, 255});
    CHECK(pixels->rgba8_at(49, 0) == Rgba{0, 255, 0, 255});
    CHECK(pixels->rgba8_at(0, 19) == Rgba{255, 0, 0, 255});
    CHECK(pixels->rgba8_at(49, 19) == Rgba{0, 0, 0, 255});
    CHECK(pixels->rgba8_at(24, 9) == Rgba{255, 255, 0, 255});
    CHECK(pixels->rgba8_at(25, 10) == Rgba{0, 0, 0, 255});
}

TEST_CASE("Compute pipeline dispatches through a frame", "[render][gpu][gpu-pass]") {
    auto rig = make_rig(true);
    if (!rig)
        SKIP(kNoDawn);
    auto& device = rig->device;
    auto shader = device.create_shader_module(R"wgsl(
@group(0) @binding(0) var<storage, read_write> values: array<u32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x < arrayLength(&values)) {
        values[id.x] = id.x * 2u;
    }
}
)wgsl",
                                              "double.wgsl");
    REQUIRE(shader);
    auto pipeline = device.create_compute_pipeline({.module = shader.value()});
    if (!pipeline)
        FAIL(pipeline.error().describe());
    auto values = device.create_buffer(
        {.size = 256 * 4, .usage = GpuBufferUsage::storage | GpuBufferUsage::copy_src});
    REQUIRE(values);
    auto group = device.create_bind_group(pipeline.value(), 0, {GpuBinding::of(0, values.value())});
    REQUIRE(group);

    GpuFrame frame(device, "compute");
    auto pass = frame.compute_pass("double");
    pass.set_pipeline(pipeline.value());
    pass.set_bind_group(0, group.value());
    pass.dispatch(4);
    pass.end();
    auto submission = frame.submit();
    if (!submission)
        FAIL(submission.error().describe());
    CHECK(submission->wait(std::chrono::seconds(5)) == GpuSubmissionStatus::succeeded);
}

TEST_CASE("A pass result composites through the Skia canvas", "[render][gpu][gpu-pass][headless]") {
    constexpr std::uint32_t kW = 64;
    constexpr std::uint32_t kH = 32;
    auto rig = make_rig(false, kW, kH);
    if (!rig)
        SKIP(kNoNativeGpu);
    SkiaSurface::Config skia_config;
    skia_config.width = kW;
    skia_config.height = kH;
    auto skia = SkiaSurface::create(*rig->surface, skia_config);
    if (!skia || !skia->is_available())
        SKIP("Skia Graphite is unavailable on this host");
    auto& device = rig->device;

    auto target = device.create_target({.width = 16, .height = 16});
    REQUIRE(target);
    auto pipeline = make_solid_pipeline(device, GpuFormat::rgba8_unorm);
    auto params = device.create_buffer({.size = 16});
    REQUIRE(params);
    const std::array<float, 4> orange{1.0f, 0.5f, 0.0f, 1.0f};
    REQUIRE(device.write_buffer(params.value(), 0, orange.data(), sizeof(orange)));
    auto group = device.create_bind_group(pipeline, 0, {GpuBinding::of(0, params.value())});
    REQUIRE(group);
    {
        GpuFrame frame(device, "composite source");
        auto pass = frame.render_pass(GpuRenderPassDesc::for_target(target.value()));
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, group.value());
        pass.draw(3);
        pass.end();
        REQUIRE(frame.submit());
    }

    REQUIRE(rig->surface->begin_frame());
    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    canvas->set_fill_color(pulp::canvas::Color::rgba8(0, 0, 255, 255));
    canvas->fill_rect(0, 0, kW, kH);
    // Draw through a temporary handle: the canvas keeps the texture alive
    // until Graphite is done with it.
    auto drawn = draw_gpu_texture(*canvas, target->color(), 0, 0, kW / 2.0f, kH);
    *target = GpuTarget{};
    skia->end_frame();
    if (!drawn)
        FAIL(drawn.error().describe());

    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const bool read = skia->read_current_rgba(rgba, width, height);
    rig->surface->end_frame();
    if (!read)
        SKIP("Skia readback is unavailable on this adapter");
    REQUIRE(width == kW);
    auto at = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
        return std::array<std::uint8_t, 4>{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    };
    const auto left = at(8, 16);
    INFO("left " << int(left[0]) << "," << int(left[1]) << "," << int(left[2]));
    CHECK(left[0] >= 250);
    CHECK((left[1] >= 120 && left[1] <= 136));
    CHECK(left[2] <= 5);
    const auto right = at(56, 16);
    CHECK(right[0] <= 5);
    CHECK(right[2] >= 250);
}

#if defined(PULP_HAS_SKIA)
TEST_CASE("Device loss is reported instead of hidden", "[render][gpu][gpu-pass][device-loss]") {
    auto rig = make_rig(true);
    if (!rig)
        SKIP(kNoDawn);
    auto target = rig->device.create_target({.width = 8, .height = 8});
    REQUIRE(target);

    auto* raw = static_cast<wgpu::Device*>(rig->surface->dawn_device_handle());
    REQUIRE(raw != nullptr);
    raw->Destroy();
    // Loss is delivered through the instance's events.
    for (int i = 0; i < 100 && !rig->device.is_lost(); ++i)
        rig->device.process_events();
    REQUIRE(rig->device.is_lost());

    auto texture = rig->device.create_texture({.width = 4, .height = 4});
    REQUIRE_FALSE(texture);
    CHECK(texture.error().kind == GpuError::Kind::device_lost);

    GpuFrame frame(rig->device, "after loss");
    auto submitted = frame.submit();
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().kind == GpuError::Kind::device_lost);

    auto pixels = target->read_pixels();
    REQUIRE_FALSE(pixels);
    CHECK(pixels.error().kind == GpuError::Kind::device_lost);
}
#endif
