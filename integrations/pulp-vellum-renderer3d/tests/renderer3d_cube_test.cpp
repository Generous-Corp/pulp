#include <pulp/vellum_renderer3d/renderer3d.hpp>

#include <pulp/scene/gltf_loader.hpp>

#if defined(PULP_VELLUM_RENDERER3D_LEGACY_PROBE)
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace r3d = pulp::vellum_renderer3d;

namespace {

constexpr std::array<std::uint8_t, 8> kPngMagic = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

// The macOS default Metal adapter's fingerprint of the 128x128 cube, also
// recorded by the legacy probe test and test/fixtures/scene3d/renderer3d-goldens.json.
constexpr std::uint64_t kMetalCube128Fingerprint = 15925498132503966243ULL;

void write_png(const std::string& name, const std::vector<std::uint8_t>& png) {
    const std::filesystem::path dir = PULP_VELLUM_RENDERER3D_PNG_DIR;
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

r3d::Scene3DRenderResult render_cube(std::uint32_t width, std::uint32_t height) {
    r3d::HardcodedCubeRenderConfig config;
    config.width = width;
    config.height = height;
    return r3d::Renderer3D::render_hardcoded_textured_cube(config);
}

} // namespace

TEST_CASE("Vellum Renderer3D draws the textured cube on gpu-runtime's device",
          "[vellum-renderer3d][gpu]") {
    const auto result = render_cube(128, 128);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }
    INFO(result.error);
    INFO("adapter " << result.adapter_backend_type << " " << result.adapter_name);
    if (result.adapter_backend_type == "Null") {
        SKIP("the Null adapter draws nothing, so its readback is not a frame");
    }

    REQUIRE(result.success);
    REQUIRE(r3d::Renderer3D::readback_holds_frame(result));
    REQUIRE(result.color_target_allocated);
    REQUIRE(result.depth_target_allocated);
    REQUIRE(result.vertex_buffer_uploaded);
    REQUIRE(result.index_buffer_uploaded);
    REQUIRE(result.uniform_buffer_uploaded);
    REQUIRE(result.texture_uploaded);
    REQUIRE(result.command_submitted);
    REQUIRE(result.readback_completed);
    REQUIRE_FALSE(result.scene_data_consumed);
    REQUIRE(result.rgba.size() == 128u * 128u * 4u);
    REQUIRE(result.png.size() > kPngMagic.size());
    REQUIRE(std::equal(kPngMagic.begin(), kPngMagic.end(), result.png.begin()));
    write_png("vellum-cube-128.png", result.png);

    if (result.adapter_backend_type == "Metal") {
        CHECK(r3d::rgba_fingerprint(result) == kMetalCube128Fingerprint);
    }
}

TEST_CASE("Vellum Renderer3D draws BoxTextured with the recorded Metal fingerprint",
          "[vellum-renderer3d][gpu]") {
    const auto path = std::filesystem::path(PULP_VELLUM_RENDERER3D_FIXTURE_DIR) / "BoxTextured" /
                      "BoxTextured.glb";
    const auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);
    r3d::SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;
    const auto result = r3d::Renderer3D::render_scene_data(loaded.scene, config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }
    if (result.adapter_backend_type == "Null") {
        SKIP("the Null adapter draws nothing, so its readback is not a frame");
    }
    INFO(result.error);
    REQUIRE(result.success);
    CHECK(result.primitive_count == 1);
    CHECK(result.texture_uploaded);
    CHECK(result.texture_sampler_applied);
    if (result.adapter_backend_type == "Metal") {
        // Recorded by renderer3d-goldens.json and the legacy probe test.
        CHECK(r3d::rgba_fingerprint(result) == 5845745157752120258ULL);
    }
}

TEST_CASE("Vellum Renderer3D refuses an empty scene before touching the GPU",
          "[vellum-renderer3d]") {
    const auto result = r3d::Renderer3D::render_scene_data(pulp::scene::SceneData{});
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.gpu_available);
    CHECK_FALSE(result.scene_data_consumed);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("Vellum Renderer3D refuses a zero-sized render before touching the GPU",
          "[vellum-renderer3d]") {
    const auto result = render_cube(0, 64);
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.gpu_available);
    CHECK(result.error == "Renderer3D: render size must be non-zero");
}

TEST_CASE("Vellum Renderer3D fingerprints dimensions before pixels", "[vellum-renderer3d]") {
    r3d::Scene3DRenderResult a;
    a.width = 10;
    a.height = 20;
    a.rgba.assign(10u * 20u * 4u, 0);
    r3d::Scene3DRenderResult b = a;
    b.width = 20;
    b.height = 10;
    CHECK(r3d::rgba_fingerprint(a) != r3d::rgba_fingerprint(b));
    // FNV-1a 64 of the eight dimension bytes alone, little-endian 1 and 1.
    r3d::Scene3DRenderResult one;
    one.width = 1;
    one.height = 1;
    CHECK(r3d::rgba_fingerprint(one) == 0xc8fdf27dfad4c453ULL);
}

#if defined(PULP_VELLUM_RENDERER3D_LEGACY_PROBE)
namespace {

struct LegacyFrame {
    std::map<std::string, std::string> fields;
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Runs the legacy pulp-renderer3d-probe, which still draws through
// pulp::render, and decodes the PNG it wrote back to unpremultiplied RGBA.
// `scene` is "hardcoded" or "boxtextured".
LegacyFrame render_legacy(const std::string& scene, std::uint32_t width, std::uint32_t height) {
    const std::filesystem::path dir = PULP_VELLUM_RENDERER3D_PNG_DIR;
    std::filesystem::create_directories(dir);
    const std::string stem = std::to_string(width) + "x" + std::to_string(height);
    const auto png_path = dir / ("legacy-" + scene + "-" + stem + ".png");
    const auto out_path = dir / ("legacy-" + scene + "-" + stem + ".txt");
    const std::string fixture = scene == "boxtextured" ? std::string(" --fixture \"") +
                                                             PULP_VELLUM_RENDERER3D_FIXTURE_DIR +
                                                             "/BoxTextured/BoxTextured.glb\""
                                                       : std::string();
    std::filesystem::remove(png_path);
    const std::string command =
        std::string("\"") + PULP_VELLUM_RENDERER3D_LEGACY_PROBE + "\" --scene " + scene + fixture +
        " --width " + std::to_string(width) + " --height " + std::to_string(height) +
        " --output-png \"" + png_path.string() + "\" > \"" + out_path.string() + "\" 2>/dev/null";
    LegacyFrame frame;
    if (std::system(command.c_str()) != 0)
        return frame;
    const auto text = read_file(out_path);
    std::istringstream lines(std::string(text.begin(), text.end()));
    for (std::string line; std::getline(lines, line);) {
        const auto eq = line.find('=');
        if (eq != std::string::npos)
            frame.fields[line.substr(0, eq)] = line.substr(eq + 1);
    }
    const auto png = read_file(png_path);
    auto image = SkImages::DeferredFromEncodedData(SkData::MakeWithCopy(png.data(), png.size()));
    if (!image)
        return frame;
    frame.width = static_cast<std::uint32_t>(image->width());
    frame.height = static_cast<std::uint32_t>(image->height());
    frame.rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4u);
    const auto info = SkImageInfo::Make(image->width(), image->height(), kRGBA_8888_SkColorType,
                                        kUnpremul_SkAlphaType);
    if (!image->readPixels(info, frame.rgba.data(), frame.width * 4u, 0, 0))
        frame.rgba.clear();
    return frame;
}

struct Size {
    std::uint32_t width;
    std::uint32_t height;
};

// 100 wide makes every readback row 400 bytes, padded to 512, so the
// de-padding is exercised as well as the draw.
constexpr std::array<Size, 3> kParitySizes = {{{128, 128}, {100, 75}, {256, 256}}};

// Same adapter, same Dawn, same shader: the tolerance is zero.
void check_same_frame(const std::string& name, const Size size,
                      const r3d::Scene3DRenderResult& ported, const LegacyFrame& legacy) {
    const auto legacy_error = legacy.fields.find("error");
    INFO(ported.error << " | legacy: "
                      << (legacy_error == legacy.fields.end() ? "" : legacy_error->second));
    REQUIRE(ported.success);
    REQUIRE(legacy.fields.count("success") == 1);
    REQUIRE(legacy.fields.at("success") == "true");
    CHECK(legacy.fields.at("adapter_backend_type") == ported.adapter_backend_type);
    CHECK(legacy.fields.at("adapter_name") == ported.adapter_name);
    CHECK(legacy.fields.at("rgba_fingerprint") == std::to_string(r3d::rgba_fingerprint(ported)));
    REQUIRE(legacy.width == size.width);
    REQUIRE(legacy.height == size.height);
    REQUIRE(legacy.rgba.size() == ported.rgba.size());

    const std::string stem = std::to_string(size.width) + "x" + std::to_string(size.height);
    write_png("vellum-" + name + "-" + stem + ".png", ported.png);

    std::size_t mismatched_pixels = 0;
    int max_channel_delta = 0;
    for (std::size_t i = 0; i < ported.rgba.size(); i += 4) {
        bool differs = false;
        for (std::size_t c = 0; c < 4; ++c) {
            const int delta = std::abs(int(ported.rgba[i + c]) - int(legacy.rgba[i + c]));
            max_channel_delta = std::max(max_channel_delta, delta);
            differs = differs || delta != 0;
        }
        mismatched_pixels += differs ? 1u : 0u;
    }
    INFO("mismatched pixels " << mismatched_pixels << ", max channel delta " << max_channel_delta);
    CHECK(mismatched_pixels == 0);
    CHECK(max_channel_delta == 0);
    CHECK(legacy.fields.at("distinct_color_count") == std::to_string(ported.distinct_color_count));
    CHECK(legacy.fields.at("non_transparent_pixel_count") ==
          std::to_string(ported.non_transparent_pixel_count));
    CHECK(legacy.fields.at("primitive_count") == std::to_string(ported.primitive_count));
    CHECK(legacy.fields.at("pipeline_cache_entry_count") ==
          std::to_string(ported.pipeline_cache_entry_count));
    CHECK(legacy.fields.at("scene_data_consumed") ==
          (ported.scene_data_consumed ? "true" : "false"));
    CHECK(legacy.fields.at("texture_decoded") == (ported.texture_decoded ? "true" : "false"));
}

bool skip_without_drawing_adapter(const r3d::Scene3DRenderResult& ported) {
    if (!ported.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << ported.error);
    }
    if (ported.adapter_backend_type == "Null") {
        SKIP("the Null adapter draws nothing, so there is no frame to compare");
    }
    return false;
}

} // namespace

TEST_CASE("Vellum Renderer3D cube matches the legacy renderer pixel for pixel",
          "[vellum-renderer3d][gpu][parity]") {
    for (const Size size : kParitySizes) {
        DYNAMIC_SECTION(size.width << "x" << size.height) {
            const auto ported = render_cube(size.width, size.height);
            skip_without_drawing_adapter(ported);
            check_same_frame("cube", size, ported,
                             render_legacy("hardcoded", size.width, size.height));
        }
    }
}

TEST_CASE("Vellum Renderer3D draws BoxTextured like the legacy renderer, pixel for pixel",
          "[vellum-renderer3d][gpu][parity]") {
    const auto path = std::filesystem::path(PULP_VELLUM_RENDERER3D_FIXTURE_DIR) / "BoxTextured" /
                      "BoxTextured.glb";
    const auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);
    for (const Size size : kParitySizes) {
        DYNAMIC_SECTION(size.width << "x" << size.height) {
            r3d::SceneDataRenderConfig config;
            config.width = size.width;
            config.height = size.height;
            const auto ported = r3d::Renderer3D::render_scene_data(loaded.scene, config);
            skip_without_drawing_adapter(ported);
            CHECK(ported.scene_data_consumed);
            CHECK(ported.texture_decoded);
            CHECK(ported.base_color_texture_srgb_applied);
            check_same_frame("boxtextured", size, ported,
                             render_legacy("boxtextured", size.width, size.height));
        }
    }
}
#endif
