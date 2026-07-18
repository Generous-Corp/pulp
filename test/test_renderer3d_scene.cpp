#include "test_renderer3d_shared.hpp"

TEST_CASE("Renderer3D renders external glTF buffers and images",
          "[render][scene3d][gpu]") {
    const auto dir = std::filesystem::temp_directory_path() /
        "pulp-renderer3d-external-gltf";
    std::filesystem::create_directories(dir);

    const auto bin = make_external_renderable_gltf_bin();
    write_file(dir / "mesh.bin", bin);
    write_file(dir / "checker.png", make_alpha_checker_png());
    const auto gltf_path = dir / "scene.gltf";
    write_file(gltf_path, make_external_renderable_gltf_json(bin.size()));

    auto loaded = pulp::scene::load_gltf_scene(gltf_path);
    INFO(loaded.error);
    REQUIRE(loaded.success);

    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(loaded.scene, config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.texture_uploaded);
    REQUIRE(result.texture_decoded);
    REQUIRE_FALSE(result.fallback_texture_used);
    REQUIRE(result.base_color_texture_srgb_applied);
    REQUIRE(result.texture_sampler_applied);
    REQUIRE(result.texture_sampler_clamp_s);
    REQUIRE(result.texture_sampler_clamp_t);
    REQUIRE(result.texture_sampler_linear);
    REQUIRE(result.geometry_normals_applied);
    REQUIRE(result.base_color_factor_applied);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 900);
}

TEST_CASE("Renderer3D renders a mesh below a child node", "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(make_child_node_render_scene(), config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.fallback_texture_used);
    REQUIRE(result.base_color_texture_srgb_applied);
    REQUIRE_FALSE(result.texture_sampler_applied);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.command_submitted);
    REQUIRE(result.readback_completed);
    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1200);
    REQUIRE(result.success);
}

TEST_CASE("Renderer3D applies base-color texture transform metadata",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(make_transformed_uv_render_scene(), config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.fallback_texture_used);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.base_color_transform_applied);
    REQUIRE(result.base_color_texcoord1_used);
    REQUIRE(result.command_submitted);
    REQUIRE(result.readback_completed);
    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1200);
    REQUIRE(result.success);
}

TEST_CASE("Renderer3D downgrades mipmap sampler state for single-level textures",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_mipmap_sampler_render_scene(),
        config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.fallback_texture_used);
    REQUIRE(result.texture_sampler_applied);
    REQUIRE(result.texture_sampler_linear);
    REQUIRE(result.texture_mipmap_filter_downgraded);
    REQUIRE(result.base_color_texture_srgb_applied);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D renders multiple same-state SceneData primitives",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(make_multi_primitive_render_scene(),
                                                config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 2);
    REQUIRE(result.pipeline_cache_entry_count == 1);
    REQUIRE(result.pipeline_cache_hit_count == 1);
    REQUIRE(result.fallback_texture_used);
    REQUIRE(result.base_color_factor_applied);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1400);
}

TEST_CASE("Renderer3D applies per-primitive material uniforms",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_mixed_material_primitive_render_scene(),
        config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.primitive_count == 2);
    REQUIRE(result.pipeline_cache_entry_count == 1);
    REQUIRE(result.pipeline_cache_hit_count == 1);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.double_sided_material_applied);
    const double red = average_foreground_channel(result.rgba,
                                                  config.width,
                                                  config.height,
                                                  0);
    const double green = average_foreground_channel(result.rgba,
                                                    config.width,
                                                    config.height,
                                                    1);
    const double blue = average_foreground_channel(result.rgba,
                                                   config.width,
                                                   config.height,
                                                   2);
    REQUIRE(red > 45.0);
    REQUIRE(green > 45.0);
    REQUIRE(blue < red * 0.35);
    REQUIRE(blue < green * 0.35);
}

TEST_CASE("Renderer3D reports a feature one SceneData primitive uses",
          "[render][scene3d][gpu]") {
    // Each primitive carries exactly one of the two features: the first is
    // unlit and single-sided, the second is lit and double-sided. The scene-wide
    // flags report a feature that any single primitive uses, and the differing
    // cull state keeps the two primitives off each other's pipeline.
    auto scene = make_mixed_material_primitive_render_scene();
    REQUIRE(scene.materials.size() == 2);
    scene.materials[0].unlit = true;
    scene.materials[0].double_sided = false;
    scene.materials[1].unlit = false;
    scene.materials[1].double_sided = true;

    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(scene, config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.primitive_count == 2);
    REQUIRE(result.unlit_material_applied);
    REQUIRE(result.double_sided_material_applied);
    REQUIRE(result.pipeline_cache_entry_count == 2);
    REQUIRE(result.pipeline_cache_hit_count == 0);
}

TEST_CASE("Renderer3D applies directional light color",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_directional_light_render_scene(),
        config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.directional_light_applied);
    REQUIRE(result.base_color_factor_applied);
    const double red = average_foreground_channel(result.rgba,
                                                  config.width,
                                                  config.height,
                                                  0);
    const double green = average_foreground_channel(result.rgba,
                                                    config.width,
                                                    config.height,
                                                    1);
    const double blue = average_foreground_channel(result.rgba,
                                                   config.width,
                                                   config.height,
                                                   2);
    REQUIRE(green > 60.0);
    REQUIRE(red < green * 0.25);
    REQUIRE(blue < green * 0.25);
}

TEST_CASE("Renderer3D applies directional light node transform",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto front_lit = Renderer3D::render_scene_data(
        make_directional_light_direction_render_scene(false),
        config);
    if (!front_lit.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: "
                << front_lit.error);
        return;
    }
    auto back_lit = Renderer3D::render_scene_data(
        make_directional_light_direction_render_scene(true),
        config);

    INFO(front_lit.error);
    INFO(back_lit.error);
    REQUIRE(front_lit.success);
    REQUIRE(back_lit.success);
    REQUIRE(front_lit.directional_light_applied);
    REQUIRE(back_lit.directional_light_applied);
    REQUIRE_FALSE(front_lit.directional_light_transform_applied);
    REQUIRE(back_lit.directional_light_transform_applied);
    REQUIRE_FALSE(back_lit.light_node_transform_deferred);
    REQUIRE(front_lit.geometry_normals_applied);
    REQUIRE(back_lit.geometry_normals_applied);

    const auto front_region = find_foreground_region(front_lit.rgba,
                                                     config.width,
                                                     config.height);
    const auto back_region = find_foreground_region(back_lit.rgba,
                                                    config.width,
                                                    config.height);
    REQUIRE(front_region.pixel_count > 1200);
    REQUIRE(back_region.pixel_count > 1200);
    REQUIRE(average_foreground_luma(front_lit.rgba,
                                    config.width,
                                    config.height) >
            average_foreground_luma(back_lit.rgba,
                                    config.width,
                                    config.height) + 20.0);
}

TEST_CASE("Renderer3D applies point and spot lights and defers range metadata",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto unlit_punctual = Renderer3D::render_scene_data(
        make_deferred_punctual_light_render_scene(false, false),
        config);
    if (!unlit_punctual.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " <<
                unlit_punctual.error);
        return;
    }
    auto baseline = Renderer3D::render_scene_data(
        make_deferred_punctual_light_render_scene(false),
        config);
    auto result = Renderer3D::render_scene_data(
        make_deferred_punctual_light_render_scene(),
        config);

    INFO(unlit_punctual.error);
    INFO(baseline.error);
    INFO(result.error);
    REQUIRE(unlit_punctual.success);
    REQUIRE(baseline.success);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE_FALSE(unlit_punctual.punctual_light_range_applied);
    REQUIRE(unlit_punctual.punctual_light_range_deferred);
    REQUIRE_FALSE(baseline.point_light_applied);
    REQUIRE(baseline.point_light_deferred);
    REQUIRE(baseline.spot_light_applied);
    REQUIRE_FALSE(baseline.spot_light_deferred);
    REQUIRE_FALSE(baseline.spot_light_cone_deferred);
    REQUIRE(baseline.punctual_light_range_applied);
    REQUIRE(baseline.punctual_light_range_deferred);
    REQUIRE_FALSE(result.directional_light_applied);
    REQUIRE(result.point_light_applied);
    REQUIRE_FALSE(result.point_light_deferred);
    REQUIRE(result.spot_light_applied);
    REQUIRE_FALSE(result.spot_light_deferred);
    REQUIRE(result.punctual_light_range_applied);
    REQUIRE_FALSE(result.punctual_light_range_deferred);
    REQUIRE_FALSE(result.spot_light_cone_deferred);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.fallback_texture_used);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1200);
    const double red = average_foreground_channel(result.rgba,
                                                  config.width,
                                                  config.height,
                                                  0);
    const double green = average_foreground_channel(result.rgba,
                                                    config.width,
                                                    config.height,
                                                    1);
    const double blue = average_foreground_channel(result.rgba,
                                                   config.width,
                                                   config.height,
                                                   2);
    const double unlit_blue =
        average_foreground_channel(unlit_punctual.rgba,
                                   config.width,
                                   config.height,
                                   2);
    const double baseline_blue = average_foreground_channel(baseline.rgba,
                                                            config.width,
                                                            config.height,
                                                            2);
    const double baseline_red = average_foreground_channel(baseline.rgba,
                                                           config.width,
                                                           config.height,
                                                           0);
    REQUIRE(baseline_blue > unlit_blue + 10.0);
    REQUIRE(red > baseline_red + 10.0);
    REQUIRE(red > green - 4.0);
    REQUIRE(blue > unlit_blue + 10.0);
}

TEST_CASE("Renderer3D applies point light range attenuation",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto short_range = Renderer3D::render_scene_data(
        make_point_light_range_render_scene(1.5f),
        config);
    if (!short_range.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " <<
                short_range.error);
        return;
    }
    auto long_range = Renderer3D::render_scene_data(
        make_point_light_range_render_scene(12.0f),
        config);

    INFO(short_range.error);
    INFO(long_range.error);
    REQUIRE(short_range.success);
    REQUIRE(long_range.success);
    REQUIRE(short_range.point_light_applied);
    REQUIRE(long_range.point_light_applied);
    REQUIRE(short_range.punctual_light_range_applied);
    REQUIRE(long_range.punctual_light_range_applied);
    REQUIRE_FALSE(short_range.punctual_light_range_deferred);
    REQUIRE_FALSE(long_range.punctual_light_range_deferred);

    const double short_red = average_foreground_channel(short_range.rgba,
                                                        config.width,
                                                        config.height,
                                                        0);
    const double long_red = average_foreground_channel(long_range.rgba,
                                                       config.width,
                                                       config.height,
                                                       0);
    REQUIRE(long_red > short_red + 18.0);
}

TEST_CASE("Renderer3D applies preserved perspective camera yfov",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto no_camera = Renderer3D::render_scene_data(
        make_perspective_camera_render_scene(false),
        config);
    if (!no_camera.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << no_camera.error);
        return;
    }
    auto with_camera = Renderer3D::render_scene_data(
        make_perspective_camera_render_scene(true),
        config);

    INFO(no_camera.error);
    INFO(with_camera.error);
    REQUIRE(no_camera.success);
    REQUIRE(with_camera.success);
    REQUIRE_FALSE(no_camera.perspective_camera_applied);
    REQUIRE(with_camera.perspective_camera_applied);
    REQUIRE(with_camera.base_color_factor_applied);

    const auto no_camera_region = find_foreground_region(no_camera.rgba,
                                                         config.width,
                                                         config.height);
    const auto with_camera_region = find_foreground_region(with_camera.rgba,
                                                           config.width,
                                                           config.height);
    REQUIRE(no_camera_region.pixel_count > 500);
    REQUIRE(with_camera_region.pixel_count >
            no_camera_region.pixel_count + 600);
}

TEST_CASE("Renderer3D reports deferred camera and light node transforms",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_transformed_camera_light_render_scene(),
        config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.perspective_camera_applied);
    REQUIRE(result.directional_light_applied);
    REQUIRE(result.directional_light_transform_applied);
    REQUIRE(result.camera_node_translation_applied);
    REQUIRE(result.camera_node_rotation_applied);
    REQUIRE_FALSE(result.camera_node_transform_deferred);
    REQUIRE_FALSE(result.light_node_transform_deferred);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.command_submitted);
    REQUIRE(result.readback_completed);
    REQUIRE(result.rgba.size() ==
            static_cast<size_t>(config.width) * config.height * 4u);
}

TEST_CASE("Renderer3D reports non-rigid light node transforms",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto scene = make_transformed_camera_light_render_scene();
    for (auto& node : scene.nodes) {
        if (node.light == 0) {
            node.scale[0] = 2.0f;
        }
    }

    auto result = Renderer3D::render_scene_data(scene, config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.directional_light_applied);
    REQUIRE(result.directional_light_transform_applied);
    REQUIRE(result.light_node_transform_deferred);
}

TEST_CASE("Renderer3D applies camera node rotation as view basis",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto translated = Renderer3D::render_scene_data(
        make_translated_camera_render_scene(-0.25f),
        config);
    if (!translated.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: "
                << translated.error);
        return;
    }
    auto rotated = Renderer3D::render_scene_data(
        make_rotated_camera_render_scene(0.382683f, 0.92388f),
        config);

    INFO(translated.error);
    INFO(rotated.error);
    REQUIRE(translated.success);
    REQUIRE(rotated.success);
    REQUIRE(translated.perspective_camera_applied);
    REQUIRE(rotated.perspective_camera_applied);
    REQUIRE(translated.camera_node_translation_applied);
    REQUIRE(rotated.camera_node_translation_applied);
    REQUIRE_FALSE(translated.camera_node_rotation_applied);
    REQUIRE(rotated.camera_node_rotation_applied);
    REQUIRE_FALSE(rotated.camera_node_transform_deferred);

    const auto translated_region = find_foreground_region(translated.rgba,
                                                          config.width,
                                                          config.height);
    const auto rotated_region = find_foreground_region(rotated.rgba,
                                                       config.width,
                                                       config.height);
    REQUIRE(translated_region.pixel_count > 1200);
    REQUIRE(rotated_region.pixel_count > 1200);
    REQUIRE(std::abs(foreground_center_x(rotated_region) -
                     foreground_center_x(translated_region)) > 2.0);
}

TEST_CASE("Renderer3D applies rigid camera matrix transform as view basis",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto translated = Renderer3D::render_scene_data(
        make_matrix_camera_render_scene(false),
        config);
    if (!translated.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: "
                << translated.error);
        return;
    }
    auto rotated = Renderer3D::render_scene_data(
        make_matrix_camera_render_scene(true),
        config);

    INFO(translated.error);
    INFO(rotated.error);
    REQUIRE(translated.success);
    REQUIRE(rotated.success);
    REQUIRE(translated.perspective_camera_applied);
    REQUIRE(rotated.perspective_camera_applied);
    REQUIRE(translated.camera_node_translation_applied);
    REQUIRE(rotated.camera_node_translation_applied);
    REQUIRE_FALSE(translated.camera_node_rotation_applied);
    REQUIRE(rotated.camera_node_rotation_applied);
    REQUIRE_FALSE(translated.camera_node_transform_deferred);
    REQUIRE_FALSE(rotated.camera_node_transform_deferred);

    const auto translated_region = find_foreground_region(translated.rgba,
                                                          config.width,
                                                          config.height);
    const auto rotated_region = find_foreground_region(rotated.rgba,
                                                       config.width,
                                                       config.height);
    REQUIRE(translated_region.pixel_count > 1200);
    REQUIRE(rotated_region.pixel_count > 1200);
    REQUIRE(std::abs(foreground_center_x(rotated_region) -
                     foreground_center_x(translated_region)) > 2.0);
}

TEST_CASE("Renderer3D applies camera node translation as a view offset",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto centered = Renderer3D::render_scene_data(
        make_perspective_camera_render_scene(true),
        config);
    if (!centered.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: "
                << centered.error);
        return;
    }
    auto shifted = Renderer3D::render_scene_data(
        make_translated_camera_render_scene(-0.25f),
        config);

    INFO(centered.error);
    INFO(shifted.error);
    REQUIRE(centered.success);
    REQUIRE(shifted.success);
    REQUIRE(centered.perspective_camera_applied);
    REQUIRE(shifted.perspective_camera_applied);
    REQUIRE_FALSE(centered.camera_node_translation_applied);
    REQUIRE(shifted.camera_node_translation_applied);
    REQUIRE_FALSE(shifted.camera_node_transform_deferred);

    const auto centered_region = find_foreground_region(centered.rgba,
                                                        config.width,
                                                        config.height);
    const auto shifted_region = find_foreground_region(shifted.rgba,
                                                       config.width,
                                                       config.height);
    REQUIRE(centered_region.pixel_count > 1200);
    REQUIRE(shifted_region.pixel_count > 1200);
    REQUIRE(shifted_region.min_x > centered_region.min_x + 8);
}

TEST_CASE("Renderer3D applies preserved camera projection metadata",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_camera_metadata_render_scene(),
        config);
    if (!result.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.perspective_camera_applied);
    REQUIRE(result.camera_aspect_ratio_applied);
    REQUIRE_FALSE(result.camera_aspect_ratio_deferred);
    REQUIRE(result.camera_depth_range_applied);
    REQUIRE_FALSE(result.camera_depth_range_deferred);
    REQUIRE(result.base_color_factor_applied);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D applies preserved camera aspect ratio",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto square = Renderer3D::render_scene_data(
        make_camera_aspect_ratio_render_scene(1.0f),
        config);
    if (!square.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << square.error);
        return;
    }
    auto wide = Renderer3D::render_scene_data(
        make_camera_aspect_ratio_render_scene(2.0f),
        config);

    INFO(square.error);
    INFO(wide.error);
    REQUIRE(square.success);
    REQUIRE(wide.success);
    REQUIRE(square.perspective_camera_applied);
    REQUIRE(wide.perspective_camera_applied);
    REQUIRE(square.camera_aspect_ratio_applied);
    REQUIRE(wide.camera_aspect_ratio_applied);
    REQUIRE_FALSE(square.camera_aspect_ratio_deferred);
    REQUIRE_FALSE(wide.camera_aspect_ratio_deferred);

    const auto square_region = find_foreground_region(square.rgba,
                                                      config.width,
                                                      config.height);
    const auto wide_region = find_foreground_region(wide.rgba,
                                                    config.width,
                                                    config.height);
    REQUIRE(square_region.pixel_count > 1200);
    REQUIRE(wide_region.pixel_count > 900);
    REQUIRE(foreground_width(square_region) >
            foreground_width(wide_region) + 12u);
}

TEST_CASE("Renderer3D applies preserved orthographic camera ymag",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto wide = Renderer3D::render_scene_data(
        make_orthographic_camera_render_scene(3.0f),
        config);
    if (!wide.gpu_available) {
        SUCCEED("Dawn/WebGPU unavailable in this environment: " << wide.error);
        return;
    }
    auto narrow = Renderer3D::render_scene_data(
        make_orthographic_camera_render_scene(1.2f),
        config);

    INFO(wide.error);
    INFO(narrow.error);
    REQUIRE(wide.success);
    REQUIRE(narrow.success);
    REQUIRE_FALSE(wide.perspective_camera_applied);
    REQUIRE_FALSE(narrow.perspective_camera_applied);
    REQUIRE(wide.orthographic_camera_applied);
    REQUIRE(narrow.orthographic_camera_applied);
    REQUIRE(wide.base_color_factor_applied);
    REQUIRE(narrow.base_color_factor_applied);

    const auto wide_region = find_foreground_region(wide.rgba,
                                                    config.width,
                                                    config.height);
    const auto narrow_region = find_foreground_region(narrow.rgba,
                                                      config.width,
                                                      config.height);
    REQUIRE(narrow_region.pixel_count > wide_region.pixel_count + 400);
}
