// CPU tests of the material projection and the scene analysis. They include
// the package's private headers and need no GPU.
#include "renderer3d_analysis.hpp"
#include "renderer3d_materials.hpp"

#include <pulp/scene/gltf_loader.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace d = pulp::vellum_renderer3d::detail;
namespace r3d = pulp::vellum_renderer3d;
using Catch::Approx;

namespace {

pulp::scene::SceneData load_box_textured() {
    const auto path = std::filesystem::path(PULP_VELLUM_RENDERER3D_FIXTURE_DIR) / "BoxTextured" /
                      "BoxTextured.glb";
    auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);
    return loaded.scene;
}

} // namespace

TEST_CASE("BoxTextured analyzes to one textured, lit primitive",
          "[vellum-renderer3d][materials-cpu]") {
    const auto scene = load_box_textured();
    auto render_scene = scene;
    r3d::Scene3DRenderResult result;
    auto analysis = d::analyze_scene_data(scene, render_scene, result);
    INFO(result.error);
    REQUIRE(analysis.has_value());
    CHECK(result.scene_data_consumed);
    CHECK(result.primitive_count == 1);
    REQUIRE(analysis->primitives.size() == 1);
    const auto& primitive = analysis->primitives.front();
    CHECK(primitive.vertices.size() == 24);
    CHECK(primitive.indices.size() == 36);
    // The fixture's one texture is a 256x256 opaque PNG.
    CHECK(primitive.texture.decoded);
    CHECK_FALSE(primitive.texture.fallback);
    CHECK(primitive.texture.width == 256);
    CHECK(primitive.texture.height == 256);
    CHECK(primitive.texture.rgba.size() == 256u * 256u * 4u);
    CHECK(primitive.geometry_normals_applied);
    CHECK(result.texture_decoded);
    CHECK_FALSE(result.fallback_texture_used);
    CHECK(result.geometry_normals_applied);
    CHECK(result.base_color_factor_applied);
    // The normalized geometry fits the renderer's unit view volume.
    for (const auto& vertex : primitive.vertices) {
        CHECK(std::abs(vertex.x) <= 1.0f);
        CHECK(std::abs(vertex.y) <= 1.0f);
        CHECK(std::abs(vertex.z) <= 1.0f);
    }
}

TEST_CASE("A texture that does not decode falls back to the checker",
          "[vellum-renderer3d][materials-cpu]") {
    auto scene = load_box_textured();
    REQUIRE_FALSE(scene.textures.empty());
    scene.textures.front().encoded_bytes.assign(64, 0x5a);
    CHECK_FALSE(d::decode_texture_rgba(scene.textures.front()).has_value());

    auto render_scene = scene;
    r3d::Scene3DRenderResult result;
    auto analysis = d::analyze_scene_data(scene, render_scene, result);
    REQUIRE(analysis.has_value());
    const auto& texture = analysis->primitives.front().texture;
    CHECK(texture.fallback);
    CHECK_FALSE(texture.decoded);
    CHECK(texture.width == 4);
    CHECK(texture.height == 4);
    CHECK(result.fallback_texture_used);
    CHECK_FALSE(result.texture_decoded);
}

TEST_CASE("An empty scene has nothing to analyze", "[vellum-renderer3d][materials-cpu]") {
    pulp::scene::SceneData scene;
    auto render_scene = scene;
    r3d::Scene3DRenderResult result;
    CHECK_FALSE(d::analyze_scene_data(scene, render_scene, result).has_value());
    CHECK_FALSE(result.scene_data_consumed);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("Sampler wrap and filter modes map to WebGPU", "[vellum-renderer3d][materials-cpu]") {
    using Wrap = pulp::scene::TextureSamplerData::Wrap;
    using Filter = pulp::scene::TextureSamplerData::Filter;
    CHECK(d::address_mode_from_scene_wrap(Wrap::clamp_to_edge) == wgpu::AddressMode::ClampToEdge);
    CHECK(d::address_mode_from_scene_wrap(Wrap::mirrored_repeat) ==
          wgpu::AddressMode::MirrorRepeat);
    CHECK(d::address_mode_from_scene_wrap(Wrap::repeat) == wgpu::AddressMode::Repeat);
    CHECK(d::filter_mode_from_scene_filter(Filter::nearest, wgpu::FilterMode::Linear) ==
          wgpu::FilterMode::Nearest);
    CHECK(d::filter_mode_from_scene_filter(Filter::unspecified, wgpu::FilterMode::Linear) ==
          wgpu::FilterMode::Linear);
}

TEST_CASE("Tangents follow the normal-texture u axis", "[vellum-renderer3d][materials-cpu]") {
    std::vector<d::SceneVertex> vertices(3);
    vertices[1].x = 1.0f;
    vertices[1].normal_u = 1.0f;
    vertices[2].y = 1.0f;
    vertices[2].normal_v = 1.0f;
    const std::vector<uint32_t> indices = {0, 1, 2};
    REQUIRE(d::derive_tangents_from_positions_normals_uvs(vertices, indices));
    for (const auto& vertex : vertices) {
        CHECK(vertex.tx == Approx(1.0f));
        CHECK(vertex.ty == Approx(0.0f).margin(1e-6));
        CHECK(vertex.tz == Approx(0.0f).margin(1e-6));
        CHECK(vertex.tw == 1.0f);
    }
}
