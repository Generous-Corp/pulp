// CPU tests of the scene metadata the renderer reads before it draws. They
// include the package's private header, so they need no GPU.
#include "renderer3d_scene.hpp"

#include <pulp/scene/render_packet.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace d = pulp::vellum_renderer3d::detail;
using Catch::Approx;

namespace {

pulp::scene::SceneData scene_with_light_node(pulp::scene::LightData light) {
    pulp::scene::SceneData scene;
    scene.lights.push_back(light);
    pulp::scene::NodeData node;
    node.light = 0;
    scene.nodes.push_back(node);
    scene.root_nodes.push_back(0);
    return scene;
}

} // namespace

TEST_CASE("Directional light follows its node and clamps its colour",
          "[vellum-renderer3d][scene-cpu]") {
    pulp::scene::LightData light;
    light.type = pulp::scene::LightData::Type::directional;
    light.color[0] = 1.0f;
    light.color[1] = 0.25f;
    light.color[2] = 0.0f;
    light.intensity = 10.0f;
    auto scene = scene_with_light_node(light);
    // A quarter turn about +Y takes the light's local +Z to +X.
    const float h = std::sqrt(0.5f);
    scene.nodes[0].rotation[1] = h;
    scene.nodes[0].rotation[3] = h;

    const auto packet = pulp::scene::build_render_packet(scene);
    const auto out = d::first_directional_light(scene, packet.transformed_nodes);
    CHECK(out.applied);
    CHECK(out.transform_applied);
    CHECK(out.direction[0] == Approx(1.0f).margin(1e-5));
    CHECK(out.direction[1] == Approx(0.0f).margin(1e-5));
    CHECK(out.direction[2] == Approx(0.0f).margin(1e-5));
    CHECK(out.color[0] == Approx(4.0f));
    CHECK(out.color[1] == Approx(2.5f));
    CHECK(out.color[2] == Approx(0.0f));
}

TEST_CASE("A light with no node keeps the default direction", "[vellum-renderer3d][scene-cpu]") {
    pulp::scene::SceneData scene;
    pulp::scene::LightData light;
    light.type = pulp::scene::LightData::Type::directional;
    scene.lights.push_back(light);
    const auto out = d::first_directional_light(scene, {});
    CHECK(out.applied);
    CHECK_FALSE(out.transform_applied);
    CHECK(out.direction[0] == Approx(0.25f));
    CHECK(out.direction[2] == Approx(0.90f));
}

TEST_CASE("Point and spot lights are reported as deferred with their ranges and cones",
          "[vellum-renderer3d][scene-cpu]") {
    pulp::scene::SceneData scene;
    pulp::scene::LightData point;
    point.type = pulp::scene::LightData::Type::point;
    point.range = 3.0f;
    pulp::scene::LightData spot;
    spot.type = pulp::scene::LightData::Type::spot;
    pulp::scene::LightData coned = spot;
    coned.inner_cone_angle = 0.2f;
    coned.range = 1.0f;
    scene.lights = {point, spot, coned};
    const auto out = d::deferred_punctual_lights(scene);
    CHECK(out.point);
    CHECK(out.spot);
    CHECK(out.point_range_count == 1);
    CHECK(out.spot_range_count == 1);
    CHECK(out.spot_cone);
}

TEST_CASE("Perspective camera projection reads its node and depth range",
          "[vellum-renderer3d][scene-cpu]") {
    pulp::scene::SceneData scene;
    pulp::scene::CameraData camera;
    camera.projection = pulp::scene::CameraData::Projection::perspective;
    camera.yfov = 0.9f;
    camera.aspect_ratio = 1.5f;
    camera.znear = 0.5f;
    camera.zfar = 0.0f;
    scene.cameras.push_back(camera);
    pulp::scene::NodeData node;
    node.camera = 0;
    node.translation[2] = 5.0f;
    scene.nodes.push_back(node);
    scene.root_nodes.push_back(0);

    d::SceneNormalization normalization;
    normalization.center[2] = 1.0f;
    normalization.scale = 0.5f;
    const auto packet = pulp::scene::build_render_packet(scene);
    const auto out = d::first_camera_projection(scene, packet.transformed_nodes, normalization);
    CHECK(out.perspective_applied);
    CHECK_FALSE(out.orthographic_applied);
    CHECK(out.camera_index == 0);
    CHECK(out.projection_scale == Approx(0.6f / std::tan(0.45f)));
    CHECK(out.aspect_ratio_applied);
    CHECK(out.aspect_ratio == Approx(1.5f));
    CHECK(out.depth_range_applied);
    CHECK(out.znear == Approx(0.5f));
    CHECK(out.zfar == Approx(100.5f));
    CHECK(out.node_translation_applied);
    CHECK(out.camera_offset[2] == Approx(2.0f));
    CHECK_FALSE(out.node_rotation_applied);
}

TEST_CASE("Orthographic camera projection scales by its magnification",
          "[vellum-renderer3d][scene-cpu]") {
    pulp::scene::SceneData scene;
    pulp::scene::CameraData camera;
    camera.projection = pulp::scene::CameraData::Projection::orthographic;
    camera.ymag = 4.0f;
    scene.cameras.push_back(camera);
    const auto out = d::first_camera_projection(scene, {}, {});
    CHECK(out.orthographic_applied);
    CHECK(out.projection_scale == Approx(0.95f / 4.0f));
}

TEST_CASE("The initial animation pose skips a cubic spline's in-tangent",
          "[vellum-renderer3d][scene-cpu]") {
    pulp::scene::SceneData scene;
    pulp::scene::NodeData node;
    node.has_matrix_transform = true;
    scene.nodes.push_back(node);
    pulp::scene::AnimationData animation;
    pulp::scene::AnimationSamplerData sampler;
    sampler.interpolation = pulp::scene::AnimationSamplerData::Interpolation::cubic_spline;
    sampler.output_components = 3;
    // in-tangent, value, out-tangent for the first keyframe
    sampler.output_values = {9.0f, 9.0f, 9.0f, 1.0f, 2.0f, 3.0f, 7.0f, 7.0f, 7.0f};
    sampler.input_times = {0.0f};
    animation.samplers.push_back(sampler);
    pulp::scene::AnimationChannelData channel;
    channel.node = 0;
    channel.sampler = 0;
    channel.path = pulp::scene::AnimationChannelData::Path::translation;
    animation.channels.push_back(channel);
    scene.animations.push_back(animation);

    CHECK(d::has_transform_animation(scene));
    CHECK(d::apply_initial_animation_pose(scene));
    CHECK_FALSE(scene.nodes[0].has_matrix_transform);
    CHECK(scene.nodes[0].translation[0] == 1.0f);
    CHECK(scene.nodes[0].translation[1] == 2.0f);
    CHECK(scene.nodes[0].translation[2] == 3.0f);
}

TEST_CASE("Unsupported glTF features map to the deferred flags", "[vellum-renderer3d][scene-cpu]") {
    pulp::scene::SceneData scene;
    scene.unsupported_features.push_back({"Skinning", "", ""});
    scene.unsupported_features.push_back({"MorphWeights", "", ""});
    const auto out = d::deferred_unsupported_features(scene);
    CHECK(out.skinning);
    CHECK(out.morph_target);
    CHECK_FALSE(out.gpu_instancing);
}

TEST_CASE("A zero direction normalizes to +Z", "[vellum-renderer3d][scene-cpu]") {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    d::normalize_direction(x, y, z);
    CHECK(x == 0.0f);
    CHECK(y == 0.0f);
    CHECK(z == 1.0f);
}
