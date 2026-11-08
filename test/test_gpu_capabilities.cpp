// Pure parts of the typed GPU pass API: the format table, tier selection,
// padded-row readback layout, WGSL diagnostic formatting and pixel access.
// No device is created; these run on every lane that builds pulp-render.

#include <catch2/catch_test_macros.hpp>

#include <pulp/render/gpu_capabilities.hpp>
#include <pulp/render/gpu_pass.hpp>
#include <pulp/render/wgsl_diagnostics.hpp>

#include "gpu_readback_layout.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace pulp::render;

namespace {

GpuDeviceFacts hardware_facts() {
    GpuDeviceFacts facts;
    facts.available = true;
    facts.adapter = GpuAdapterKind::integrated;
    facts.max_texture_dimension_2d = 8192;
    facts.max_color_attachments = 8;
    facts.max_storage_textures_per_shader_stage = 4;
    facts.max_compute_invocations_per_workgroup = 256;
    return facts;
}

} // namespace

TEST_CASE("pick_tier maps device facts to a tier", "[render][gpu-pass][tier]") {
    SECTION("no device and the Null backend render nothing") {
        GpuDeviceFacts none;
        CHECK(pick_tier(none) == GpuRenderTier::none);
        auto null_backend = hardware_facts();
        null_backend.null_backend = true;
        CHECK(pick_tier(null_backend) == GpuRenderTier::none);
    }
    SECTION("a CPU adapter is basic whatever its limits") {
        auto cpu = hardware_facts();
        cpu.adapter = GpuAdapterKind::cpu;
        cpu.max_texture_dimension_2d = 16384;
        CHECK(pick_tier(cpu) == GpuRenderTier::basic);
    }
    SECTION("hardware at the WebGPU defaults is standard") {
        CHECK(pick_tier(hardware_facts()) == GpuRenderTier::standard);
    }
    SECTION("hardware below a standard limit is basic") {
        auto low = hardware_facts();
        low.max_storage_textures_per_shader_stage = 2;
        CHECK(pick_tier(low) == GpuRenderTier::basic);
        low = hardware_facts();
        low.max_color_attachments = 2;
        CHECK(pick_tier(low) == GpuRenderTier::basic);
    }
    SECTION("a discrete adapter is high") {
        auto discrete = hardware_facts();
        discrete.adapter = GpuAdapterKind::discrete;
        CHECK(pick_tier(discrete) == GpuRenderTier::high);
    }
    SECTION("large limits with float32 blending are high on any hardware adapter") {
        auto large = hardware_facts();
        large.max_texture_dimension_2d = 16384;
        CHECK(pick_tier(large) == GpuRenderTier::standard);
        large.features.float32_blendable = true;
        CHECK(pick_tier(large) == GpuRenderTier::high);
    }
    CHECK(gpu_render_tier_name(GpuRenderTier::standard) == "standard");
}

TEST_CASE("format table follows WebGPU and its optional features", "[render][gpu-pass][formats]") {
    GpuDeviceFeatures base;
    CHECK(gpu_format_support(GpuFormat::rgba8_unorm, base).storage);
    CHECK(gpu_format_support(GpuFormat::rgba8_unorm, base).multisample);
    CHECK_FALSE(gpu_format_support(GpuFormat::bgra8_unorm, base).storage);
    CHECK_FALSE(gpu_format_support(GpuFormat::rg11b10_ufloat, base).renderable);
    CHECK_FALSE(gpu_format_support(GpuFormat::rgba32_float, base).multisample);
    CHECK_FALSE(gpu_format_support(GpuFormat::r32_float, base).blendable);
    CHECK_FALSE(gpu_format_support(GpuFormat::r32_uint, base).blendable);

    GpuDeviceFeatures extended;
    extended.bgra8_unorm_storage = true;
    extended.rg11b10_ufloat_renderable = true;
    extended.float32_blendable = true;
    extended.float32_filterable = true;
    CHECK(gpu_format_support(GpuFormat::bgra8_unorm, extended).storage);
    CHECK(gpu_format_support(GpuFormat::rg11b10_ufloat, extended).renderable);
    CHECK(gpu_format_support(GpuFormat::r32_float, extended).blendable);
    CHECK(gpu_format_support(GpuFormat::rgba32_float, extended).filterable);

    GpuDeviceCapabilities caps;
    fill_format_table(caps);
    REQUIRE(caps.formats.size() == all_gpu_formats().size());
    CHECK(caps.sample_counts(GpuFormat::rgba8_unorm) == std::vector<std::uint32_t>{1, 4});
    CHECK(caps.sample_counts(GpuFormat::rgba32_float) == std::vector<std::uint32_t>{1});
    CHECK(caps.sample_counts(GpuFormat::rg11b10_ufloat).empty());
    for (auto format : caps.renderable_color_formats())
        CHECK_FALSE(gpu_format_is_depth(format));
    const auto storage = caps.storage_formats();
    CHECK(std::find(storage.begin(), storage.end(), GpuFormat::rgba8_unorm) != storage.end());
    CHECK(std::find(storage.begin(), storage.end(), GpuFormat::bgra8_unorm) == storage.end());

    CHECK(gpu_format_name(GpuFormat::depth24_plus_stencil8) == "depth24plus-stencil8");
    CHECK(gpu_format_bytes_per_pixel(GpuFormat::rgba16_float) == 8);
    CHECK(gpu_format_bytes_per_pixel(GpuFormat::depth32_float) == 0);
}

TEST_CASE("padded readback rows compact back to tight rows", "[render][gpu-pass][readback]") {
    using detail::padded_row_layout;
    static_assert(padded_row_layout(64, 1, 4).padded_bytes_per_row == 256);
    static_assert(padded_row_layout(65, 1, 4).padded_bytes_per_row == 512);

    const auto layout = padded_row_layout(3, 2, 4);
    CHECK(layout.compact_bytes_per_row == 12);
    CHECK(layout.padded_bytes_per_row == 256);
    CHECK(layout.buffer_size == 512);

    std::vector<std::uint8_t> padded(layout.buffer_size, 0xEE);
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t i = 0; i < 12; ++i)
            padded[y * 256 + i] = static_cast<std::uint8_t>(y * 100 + i);
    const auto tight = detail::compact_padded_rows(padded.data(), layout);
    REQUIRE(tight.size() == 24);
    CHECK(tight[0] == 0);
    CHECK(tight[11] == 11);
    CHECK(tight[12] == 100);
    CHECK(tight[23] == 111);
}

TEST_CASE("WGSL diagnostics format as file:line:column", "[render][gpu-pass][wgsl]") {
    WgslDiagnostic positioned;
    positioned.line = 3;
    positioned.column = 14;
    positioned.message = "unresolved identifier 'colour'";
    CHECK(format_wgsl_diagnostic("glow.wgsl", positioned) ==
          "glow.wgsl:3:14: error: unresolved identifier 'colour'");

    WgslDiagnostic unpositioned;
    unpositioned.severity = WgslDiagnostic::Severity::warning;
    unpositioned.message = "unused";
    CHECK(format_wgsl_diagnostic("", unpositioned) == "<wgsl>: warning: unused");

    CHECK(has_wgsl_errors({positioned, unpositioned}));
    CHECK_FALSE(has_wgsl_errors({unpositioned}));
    CHECK(format_wgsl_diagnostics("a.wgsl", {positioned, unpositioned}) ==
          "a.wgsl:3:14: error: unresolved identifier 'colour'\na.wgsl: warning: unused");

    GpuError error;
    error.kind = GpuError::Kind::shader_compilation;
    error.message = "WGSL compilation failed";
    error.diagnostics = {positioned};
    CHECK(error.describe() ==
          "shader_compilation: WGSL compilation failed\n<wgsl>:3:14: error: unresolved "
          "identifier 'colour'");
}

TEST_CASE("GpuPixels reads RGBA and swizzles BGRA", "[render][gpu-pass][readback]") {
    GpuPixels pixels;
    pixels.width = 2;
    pixels.height = 1;
    pixels.bytes_per_pixel = 4;
    pixels.bytes = {1, 2, 3, 4, 5, 6, 7, 8};
    pixels.format = GpuFormat::rgba8_unorm;
    CHECK(pixels.rgba8_at(1, 0) == std::array<std::uint8_t, 4>{5, 6, 7, 8});
    pixels.format = GpuFormat::bgra8_unorm;
    CHECK(pixels.rgba8_at(0, 0) == std::array<std::uint8_t, 4>{3, 2, 1, 4});
    CHECK(pixels.rgba8_at(2, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    pixels.format = GpuFormat::rgba16_float;
    CHECK(pixels.rgba8_at(0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
}

TEST_CASE("an unwrapped device refuses every call with unavailable", "[render][gpu-pass]") {
    GpuDevice device;
    CHECK_FALSE(device.valid());
    CHECK(device.is_lost());
    auto texture = device.create_texture({.width = 4, .height = 4});
    REQUIRE_FALSE(texture);
    CHECK(texture.error().kind == GpuError::Kind::unavailable);
    auto shader = device.create_shader_module("@compute @workgroup_size(1) fn main() {}");
    REQUIRE_FALSE(shader);
    CHECK(shader.error().kind == GpuError::Kind::unavailable);

    GpuFrame frame(device, "no device");
    auto pass = frame.render_pass({});
    CHECK_FALSE(pass.valid());
    auto submitted = frame.submit();
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().kind == GpuError::Kind::unavailable);
    CHECK(frame.submitted());
    auto again = frame.submit();
    REQUIRE_FALSE(again);
}
