#include "test_renderer3d_shared.hpp"

TEST_CASE("Renderer3D applies transform animation initial pose and defers playback",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto static_scene = make_transform_animation_render_scene(0.0f);
    static_scene.animations.clear();
    auto static_result = Renderer3D::render_scene_data(static_scene, config);
    if (!static_result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: "
                << static_result.error);
    }

    auto result = Renderer3D::render_scene_data(
        make_transform_animation_render_scene(),
        config);

    INFO(static_result.error);
    INFO(result.error);
    REQUIRE(static_result.success);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.transform_animation_initial_pose_applied);
    REQUIRE(result.transform_animation_deferred);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.fallback_texture_used);
    REQUIRE_FALSE(static_result.transform_animation_initial_pose_applied);
    REQUIRE_FALSE(static_result.transform_animation_deferred);
    const auto static_foreground =
        find_foreground_region(static_result.rgba, config.width, config.height);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(static_foreground.pixel_count > 1200);
    REQUIRE(foreground.pixel_count > 1200);
    const HeadlessSurface::Rgba static_rgba{
        static_result.rgba,
        static_result.width,
        static_result.height,
    };
    const HeadlessSurface::Rgba animated_rgba{
        result.rgba,
        result.width,
        result.height,
    };
    REQUIRE(HeadlessSurface::rgba_fingerprint(animated_rgba) !=
            HeadlessSurface::rgba_fingerprint(static_rgba));
}

TEST_CASE("Renderer3D applies rotation and scale animation initial poses",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto assert_initial_pose_changes_render =
        [&](pulp::scene::SceneData scene) {
            auto static_scene = scene;
            static_scene.animations.clear();
            auto static_result =
                Renderer3D::render_scene_data(static_scene, config);
            if (!static_result.gpu_available) {
                SKIP("Dawn/WebGPU unavailable in this environment: "
                        << static_result.error);
            }

            auto result = Renderer3D::render_scene_data(scene, config);

            INFO(static_result.error);
            INFO(result.error);
            REQUIRE(static_result.success);
            REQUIRE(result.success);
            REQUIRE(result.transform_animation_initial_pose_applied);
            REQUIRE(result.transform_animation_deferred);
            REQUIRE_FALSE(static_result.transform_animation_initial_pose_applied);
            REQUIRE_FALSE(static_result.transform_animation_deferred);

            const auto static_foreground =
                find_foreground_region(static_result.rgba,
                                       config.width,
                                       config.height);
            const auto foreground =
                find_foreground_region(result.rgba, config.width, config.height);
            REQUIRE(static_foreground.pixel_count > 1200);
            REQUIRE(foreground.pixel_count > 1200);

            const HeadlessSurface::Rgba static_rgba{
                static_result.rgba,
                static_result.width,
                static_result.height,
            };
            const HeadlessSurface::Rgba animated_rgba{
                result.rgba,
                result.width,
                result.height,
            };
            REQUIRE(HeadlessSurface::rgba_fingerprint(animated_rgba) !=
                    HeadlessSurface::rgba_fingerprint(static_rgba));
        };

    SECTION("cubic spline rotation") {
        assert_initial_pose_changes_render(make_rotation_animation_render_scene());
    }

    SECTION("scale") {
        assert_initial_pose_changes_render(make_scale_animation_render_scene());
    }
}

TEST_CASE("Renderer3D reports deferred unsupported glTF feature records",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_unsupported_feature_deferred_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.skinning_deferred);
    REQUIRE(result.morph_target_deferred);
    REQUIRE(result.gpu_instancing_deferred);
    REQUIRE(result.perspective_camera_applied);
    REQUIRE(result.base_color_factor_applied);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D reports deferred advanced material extensions",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_advanced_material_extension_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.advanced_material_extension_deferred);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.double_sided_material_applied);
    REQUIRE(result.fallback_texture_used);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D applies unlit material shading",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto lit = Renderer3D::render_scene_data(make_unlit_render_scene(false), config);
    if (!lit.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << lit.error);
    }
    auto unlit = Renderer3D::render_scene_data(make_unlit_render_scene(true), config);

    INFO(lit.error);
    INFO(unlit.error);
    REQUIRE(lit.success);
    REQUIRE(unlit.success);
    REQUIRE_FALSE(lit.unlit_material_applied);
    REQUIRE(unlit.unlit_material_applied);
    REQUIRE(lit.fallback_texture_used);
    REQUIRE(unlit.fallback_texture_used);
    REQUIRE(average_foreground_luma(unlit.rgba, config.width, config.height) >
            average_foreground_luma(lit.rgba, config.width, config.height) + 10.0);
}

TEST_CASE("Renderer3D applies alpha-mask material cutoff",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto opaque = Renderer3D::render_scene_data(make_alpha_mask_render_scene(false),
                                                config);
    if (!opaque.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << opaque.error);
    }
    auto masked = Renderer3D::render_scene_data(make_alpha_mask_render_scene(true),
                                                config);

    INFO(opaque.error);
    INFO(masked.error);
    REQUIRE(opaque.success);
    REQUIRE(masked.success);
    REQUIRE(opaque.texture_decoded);
    REQUIRE(masked.texture_decoded);
    REQUIRE_FALSE(opaque.alpha_mask_applied);
    REQUIRE(masked.alpha_mask_applied);
    const auto opaque_foreground = find_foreground_region(opaque.rgba,
                                                          config.width,
                                                          config.height);
    const auto masked_foreground = find_foreground_region(masked.rgba,
                                                          config.width,
                                                          config.height);
    REQUIRE(masked_foreground.pixel_count < opaque_foreground.pixel_count);
}

TEST_CASE("Renderer3D applies vertex colors",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(make_vertex_color_render_scene(),
                                                config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.vertex_color_applied);
    REQUIRE(result.fallback_texture_used);
    const double red = average_foreground_channel(result.rgba, config.width, config.height, 0);
    const double green = average_foreground_channel(result.rgba, config.width, config.height, 1);
    const double blue = average_foreground_channel(result.rgba, config.width, config.height, 2);
    REQUIRE(red > 40.0);
    REQUIRE(green < red * 0.25);
    REQUIRE(blue < red * 0.25);
}

TEST_CASE("Renderer3D applies geometry normals to lighting",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto front = Renderer3D::render_scene_data(
        make_geometry_normal_render_scene(true),
        config);
    if (!front.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << front.error);
    }
    auto back = Renderer3D::render_scene_data(
        make_geometry_normal_render_scene(false),
        config);

    INFO(front.error);
    INFO(back.error);
    REQUIRE(front.success);
    REQUIRE(back.success);
    REQUIRE(front.geometry_normals_applied);
    REQUIRE(back.geometry_normals_applied);
    REQUIRE(front.fallback_texture_used);
    REQUIRE(back.fallback_texture_used);
    const double front_luma = average_image_luma(front.rgba,
                                                 config.width,
                                                 config.height);
    const double back_luma = average_image_luma(back.rgba,
                                                config.width,
                                                config.height);
    REQUIRE(front_luma > back_luma + 5.0);
}

TEST_CASE("Renderer3D reports deferred normal-map inputs",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_normal_texture_deferred_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.geometry_normals_applied);
    REQUIRE(result.tangent_attributes_available);
    REQUIRE(result.normal_texture_deferred);
    REQUIRE(result.normal_scale_deferred);
    REQUIRE(result.fallback_texture_used);
    REQUIRE_FALSE(result.texture_decoded);
    REQUIRE(result.base_color_factor_applied);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D samples normal material texture",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto plain = Renderer3D::render_scene_data(
        make_geometry_normal_render_scene(true),
        config);
    if (!plain.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << plain.error);
    }
    auto normal_mapped = Renderer3D::render_scene_data(
        make_normal_texture_render_scene(),
        config);

    INFO(plain.error);
    INFO(normal_mapped.error);
    REQUIRE(plain.success);
    REQUIRE(normal_mapped.success);
    REQUIRE_FALSE(plain.normal_texture_applied);
    REQUIRE(normal_mapped.normal_texture_applied);
    REQUIRE_FALSE(normal_mapped.normal_scale_applied);
    REQUIRE_FALSE(normal_mapped.normal_texture_deferred);
    REQUIRE_FALSE(normal_mapped.normal_scale_deferred);
    REQUIRE(normal_mapped.texture_decoded);
    REQUIRE(normal_mapped.fallback_texture_used);
    const double plain_luma = average_image_luma(plain.rgba,
                                                 config.width,
                                                 config.height);
    const double normal_luma = average_image_luma(normal_mapped.rgba,
                                                  config.width,
                                                  config.height);
    REQUIRE(normal_luma < plain_luma - 5.0);
}

TEST_CASE("Renderer3D applies normal texture scale",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto default_scale = Renderer3D::render_scene_data(
        make_normal_texture_render_scene(1.0f, true),
        config);
    if (!default_scale.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: "
                << default_scale.error);
    }
    auto boosted_scale = Renderer3D::render_scene_data(
        make_normal_texture_render_scene(2.0f, true),
        config);

    INFO(default_scale.error);
    INFO(boosted_scale.error);
    REQUIRE(default_scale.success);
    REQUIRE(boosted_scale.success);
    REQUIRE(default_scale.normal_texture_applied);
    REQUIRE(boosted_scale.normal_texture_applied);
    REQUIRE_FALSE(default_scale.normal_scale_applied);
    REQUIRE(boosted_scale.normal_scale_applied);
    REQUIRE_FALSE(boosted_scale.normal_texture_deferred);
    REQUIRE_FALSE(boosted_scale.normal_scale_deferred);
    const HeadlessSurface::Rgba default_rgba{
        default_scale.rgba,
        default_scale.width,
        default_scale.height,
    };
    const HeadlessSurface::Rgba boosted_rgba{
        boosted_scale.rgba,
        boosted_scale.width,
        boosted_scale.height,
    };
    REQUIRE(HeadlessSurface::rgba_fingerprint(boosted_rgba) !=
            HeadlessSurface::rgba_fingerprint(default_rgba));
}

TEST_CASE("Renderer3D derives tangents for normal material texture",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto plain = Renderer3D::render_scene_data(
        make_geometry_normal_render_scene(true),
        config);
    if (!plain.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << plain.error);
    }
    auto normal_mapped = Renderer3D::render_scene_data(
        make_normal_texture_derived_tangent_render_scene(),
        config);

    INFO(plain.error);
    INFO(normal_mapped.error);
    REQUIRE(plain.success);
    REQUIRE(normal_mapped.success);
    REQUIRE_FALSE(plain.normal_texture_applied);
    REQUIRE(normal_mapped.tangent_attributes_available);
    REQUIRE(normal_mapped.tangent_attributes_derived);
    REQUIRE(normal_mapped.normal_texture_applied);
    REQUIRE(normal_mapped.normal_scale_applied);
    REQUIRE_FALSE(normal_mapped.normal_texture_deferred);
    REQUIRE_FALSE(normal_mapped.normal_scale_deferred);
    REQUIRE(normal_mapped.texture_decoded);
    REQUIRE(normal_mapped.fallback_texture_used);
    const double plain_luma = average_image_luma(plain.rgba,
                                                 config.width,
                                                 config.height);
    const double normal_luma = average_image_luma(normal_mapped.rgba,
                                                  config.width,
                                                  config.height);
    REQUIRE(normal_luma < plain_luma - 5.0);
}

TEST_CASE("Renderer3D reports remaining deferred PBR texture slots",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_pbr_texture_slots_deferred_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.geometry_normals_applied);
    REQUIRE(result.metallic_roughness_texture_applied);
    REQUIRE_FALSE(result.metallic_roughness_texture_deferred);
    REQUIRE_FALSE(result.tangent_attributes_derived);
    REQUIRE(result.normal_texture_deferred);
    REQUIRE(result.normal_scale_deferred);
    REQUIRE(result.occlusion_texture_applied);
    REQUIRE_FALSE(result.occlusion_texture_deferred);
    REQUIRE_FALSE(result.occlusion_strength_deferred);
    REQUIRE(result.emissive_texture_applied);
    REQUIRE_FALSE(result.emissive_texture_deferred);
    REQUIRE(result.non_base_color_texture_transform_applied);
    REQUIRE(result.non_base_color_texcoord1_used);
    REQUIRE_FALSE(result.non_base_color_texture_transform_deferred);
    REQUIRE_FALSE(result.non_base_color_texcoord1_deferred);
    REQUIRE(result.emissive_factor_applied);
    REQUIRE(result.fallback_texture_used);
    REQUIRE(result.texture_decoded);
    const auto foreground = find_foreground_region(result.rgba,
                                                   config.width,
                                                   config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D deferred flags stay aligned with sidecar native gaps",
          "[render][scene3d][gpu][bake]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto unsupported_scene = make_unsupported_feature_deferred_render_scene();
    auto unsupported_render = Renderer3D::render_scene_data(unsupported_scene,
                                                            config);
    if (!unsupported_render.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: "
                << unsupported_render.error);
    }
    auto unsupported_report = pulp::scene::analyze_bake_preflight(
        pulp::scene::build_sidecar_from_scene(unsupported_scene));

    REQUIRE(unsupported_render.success);
    REQUIRE(unsupported_render.skinning_deferred);
    REQUIRE(unsupported_render.morph_target_deferred);
    REQUIRE(unsupported_render.gpu_instancing_deferred);
    REQUIRE(unsupported_report.native_runtime_has_gaps);
    REQUIRE(has_native_runtime_gap(unsupported_report, "Skinning"));
    REQUIRE(has_native_runtime_gap(unsupported_report, "MorphTargets"));
    REQUIRE(has_native_runtime_gap(unsupported_report, "GpuInstancing"));
    REQUIRE(std::string(pulp::scene::bake_preflight_readiness(
                unsupported_report)) == "native_gaps");

    auto animated_scene = make_transform_animation_render_scene();
    auto animated_render = Renderer3D::render_scene_data(animated_scene,
                                                         config);
    auto animated_report = pulp::scene::analyze_bake_preflight(
        pulp::scene::build_sidecar_from_scene(animated_scene));

    REQUIRE(animated_render.success);
    REQUIRE(animated_render.transform_animation_initial_pose_applied);
    REQUIRE(animated_render.transform_animation_deferred);
    REQUIRE(animated_report.native_runtime_has_gaps);
    REQUIRE(has_native_runtime_gap(animated_report, "TransformAnimation"));
    REQUIRE(std::string(pulp::scene::bake_preflight_readiness(
                animated_report)) == "native_gaps");

    auto material_scene = make_pbr_texture_slots_deferred_render_scene();
    auto material_render = Renderer3D::render_scene_data(material_scene,
                                                         config);
    auto material_report = pulp::scene::analyze_bake_preflight(
        pulp::scene::build_sidecar_from_scene(material_scene));

    REQUIRE(material_render.success);
    REQUIRE(material_render.normal_texture_deferred);
    REQUIRE(material_render.normal_scale_deferred);
    REQUIRE_FALSE(material_render.occlusion_strength_deferred);
    REQUIRE(material_render.non_base_color_texture_transform_applied);
    REQUIRE(material_render.non_base_color_texcoord1_used);
    REQUIRE_FALSE(material_render.non_base_color_texture_transform_deferred);
    REQUIRE_FALSE(material_render.non_base_color_texcoord1_deferred);
    REQUIRE(material_report.native_runtime_has_gaps);
    REQUIRE(has_native_runtime_gap(material_report,
                                   "MaterialTexture:normalTangents"));
    REQUIRE(has_native_runtime_gap(material_report,
                                   "MaterialTexture:normalScale"));
    REQUIRE_FALSE(has_native_runtime_gap(
        material_report,
        "MaterialTextureTransform:nonBaseColor"));
    REQUIRE_FALSE(has_native_runtime_gap(material_report,
                                         "MaterialTexcoord:nonBaseColor"));
    REQUIRE_FALSE(has_native_runtime_gap(
        material_report,
        "MaterialTexture:occlusionStrength"));
    REQUIRE(std::string(pulp::scene::bake_preflight_readiness(
                material_report)) == "native_gaps");
}

TEST_CASE("Renderer3D applies double-sided material culling",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto single_sided = Renderer3D::render_scene_data(
        make_back_facing_render_scene(false),
        config);
    if (!single_sided.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << single_sided.error);
    }
    auto double_sided = Renderer3D::render_scene_data(
        make_back_facing_render_scene(true),
        config);

    INFO(single_sided.error);
    INFO(double_sided.error);
    REQUIRE_FALSE(single_sided.double_sided_material_applied);
    REQUIRE(double_sided.double_sided_material_applied);
    const auto single_foreground = find_foreground_region(single_sided.rgba,
                                                          config.width,
                                                          config.height);
    const auto double_foreground = find_foreground_region(double_sided.rgba,
                                                          config.width,
                                                          config.height);
    REQUIRE(single_foreground.pixel_count < 100);
    REQUIRE(double_foreground.pixel_count > 1200);
    REQUIRE(double_sided.success);
}

TEST_CASE("Renderer3D applies alpha-blend material state",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto opaque = Renderer3D::render_scene_data(make_alpha_blend_render_scene(false),
                                                config);
    if (!opaque.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << opaque.error);
    }
    auto blended = Renderer3D::render_scene_data(make_alpha_blend_render_scene(true),
                                                 config);

    INFO(opaque.error);
    INFO(blended.error);
    REQUIRE(opaque.success);
    REQUIRE(blended.success);
    REQUIRE_FALSE(opaque.alpha_blend_applied);
    REQUIRE(blended.alpha_blend_applied);
    const double opaque_luma = average_foreground_luma(opaque.rgba,
                                                       config.width,
                                                       config.height);
    const double blended_luma = average_foreground_luma(blended.rgba,
                                                        config.width,
                                                        config.height);
    REQUIRE(blended_luma < opaque_luma * 0.65);
    REQUIRE(blended_luma > opaque_luma * 0.20);
}

TEST_CASE("Renderer3D draws opaque primitives before alpha-blend primitives",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_alpha_blend_over_opaque_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.primitive_count == 2);
    REQUIRE(result.alpha_blend_applied);
    REQUIRE(result.alpha_blend_depth_write_disabled);
    REQUIRE(result.pipeline_cache_entry_count == 2);
    REQUIRE(result.pipeline_cache_hit_count == 0);

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

TEST_CASE("Renderer3D sorts alpha-blend primitives back to front",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_alpha_blend_sorted_layers_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.primitive_count == 2);
    REQUIRE(result.alpha_blend_applied);
    REQUIRE(result.alpha_blend_depth_write_disabled);
    REQUIRE(result.alpha_blend_sorted);
    REQUIRE(result.pipeline_cache_entry_count == 1);
    REQUIRE(result.pipeline_cache_hit_count == 1);

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
    REQUIRE(red > blue * 1.20);
    REQUIRE(green < red * 0.35);
    REQUIRE(green < blue * 0.60);
}

TEST_CASE("Renderer3D applies emissive material factor",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto plain = Renderer3D::render_scene_data(make_emissive_render_scene(false),
                                               config);
    if (!plain.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << plain.error);
    }
    auto emissive = Renderer3D::render_scene_data(make_emissive_render_scene(true),
                                                  config);

    INFO(plain.error);
    INFO(emissive.error);
    REQUIRE(plain.success);
    REQUIRE(emissive.success);
    REQUIRE_FALSE(plain.emissive_factor_applied);
    REQUIRE(emissive.emissive_factor_applied);
    REQUIRE_FALSE(emissive.emissive_strength_applied);
    const double plain_blue = average_foreground_channel(plain.rgba,
                                                         config.width,
                                                         config.height,
                                                         2);
    const double emissive_blue = average_foreground_channel(emissive.rgba,
                                                            config.width,
                                                            config.height,
                                                            2);
    REQUIRE(emissive_blue > plain_blue + 80.0);
}

TEST_CASE("Renderer3D applies emissive material strength",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto baseline = Renderer3D::render_scene_data(
        make_emissive_render_scene(true, 1.0f),
        config);
    if (!baseline.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: "
                << baseline.error);
    }
    auto boosted = Renderer3D::render_scene_data(
        make_emissive_render_scene(true, 1.8f),
        config);

    INFO(baseline.error);
    INFO(boosted.error);
    REQUIRE(baseline.success);
    REQUIRE(boosted.success);
    REQUIRE(baseline.emissive_factor_applied);
    REQUIRE(boosted.emissive_factor_applied);
    REQUIRE_FALSE(baseline.emissive_strength_applied);
    REQUIRE(boosted.emissive_strength_applied);
    const double baseline_blue = average_foreground_channel(baseline.rgba,
                                                            config.width,
                                                            config.height,
                                                            2);
    const double boosted_blue = average_foreground_channel(boosted.rgba,
                                                           config.width,
                                                           config.height,
                                                           2);
    REQUIRE(boosted_blue > baseline_blue + 45.0);
}

TEST_CASE("Renderer3D samples emissive material texture",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_emissive_texture_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.emissive_factor_applied);
    REQUIRE(result.emissive_texture_applied);
    REQUIRE_FALSE(result.emissive_texture_deferred);
    REQUIRE(result.texture_decoded);
    REQUIRE(result.fallback_texture_used);
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
    REQUIRE(blue > red + 80.0);
    REQUIRE(blue > green + 80.0);
}

TEST_CASE("Renderer3D applies emissive texture transform on TEXCOORD_1",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(
        make_emissive_texture_texcoord1_transform_render_scene(),
        config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.emissive_factor_applied);
    REQUIRE(result.emissive_texture_applied);
    REQUIRE_FALSE(result.emissive_texture_deferred);
    REQUIRE(result.non_base_color_texture_transform_applied);
    REQUIRE(result.non_base_color_texcoord1_used);
    REQUIRE_FALSE(result.non_base_color_texture_transform_deferred);
    REQUIRE_FALSE(result.non_base_color_texcoord1_deferred);
    REQUIRE(result.texture_decoded);
    REQUIRE(result.fallback_texture_used);
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
    REQUIRE(blue > red + 120.0);
    REQUIRE(blue > green + 120.0);
}

TEST_CASE("Renderer3D applies metallic and roughness material factors",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto smooth_dielectric = Renderer3D::render_scene_data(
        make_metallic_roughness_factor_render_scene(0.0f, 0.1f),
        config);
    if (!smooth_dielectric.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " <<
                smooth_dielectric.error);
    }
    auto rough_metal = Renderer3D::render_scene_data(
        make_metallic_roughness_factor_render_scene(1.0f, 1.0f),
        config);

    INFO(smooth_dielectric.error);
    INFO(rough_metal.error);
    REQUIRE(smooth_dielectric.success);
    REQUIRE(rough_metal.success);
    REQUIRE(smooth_dielectric.metallic_roughness_factor_applied);
    REQUIRE(rough_metal.metallic_roughness_factor_applied);
    REQUIRE(smooth_dielectric.fallback_texture_used);
    REQUIRE(rough_metal.fallback_texture_used);
    const double smooth_luma = average_image_luma(smooth_dielectric.rgba,
                                                  config.width,
                                                  config.height);
    const double rough_luma = average_image_luma(rough_metal.rgba,
                                                 config.width,
                                                 config.height);
    REQUIRE(smooth_luma > rough_luma + 10.0);
}

TEST_CASE("Renderer3D samples metallic-roughness material texture",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto factor_only = Renderer3D::render_scene_data(
        make_metallic_roughness_factor_render_scene(1.0f, 1.0f),
        config);
    if (!factor_only.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " <<
                factor_only.error);
    }
    auto textured = Renderer3D::render_scene_data(
        make_metallic_roughness_texture_render_scene(),
        config);

    INFO(factor_only.error);
    INFO(textured.error);
    REQUIRE(factor_only.success);
    REQUIRE(textured.success);
    REQUIRE_FALSE(factor_only.metallic_roughness_texture_applied);
    REQUIRE(textured.metallic_roughness_texture_applied);
    REQUIRE_FALSE(textured.metallic_roughness_texture_deferred);
    REQUIRE(textured.texture_decoded);
    REQUIRE(textured.fallback_texture_used);
    const double factor_luma = average_image_luma(factor_only.rgba,
                                                  config.width,
                                                  config.height);
    const double textured_luma = average_image_luma(textured.rgba,
                                                    config.width,
                                                    config.height);
    REQUIRE(textured_luma > factor_luma + 10.0);
}

TEST_CASE("Renderer3D applies metallic-roughness texture transform on TEXCOORD_1",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto untransformed = Renderer3D::render_scene_data(
        make_metallic_roughness_texcoord1_transform_render_scene(false),
        config);
    if (!untransformed.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: "
                << untransformed.error);
    }
    auto transformed = Renderer3D::render_scene_data(
        make_metallic_roughness_texcoord1_transform_render_scene(true),
        config);

    INFO(untransformed.error);
    INFO(transformed.error);
    REQUIRE(untransformed.success);
    REQUIRE(transformed.success);
    REQUIRE(untransformed.metallic_roughness_texture_applied);
    REQUIRE(transformed.metallic_roughness_texture_applied);
    REQUIRE_FALSE(transformed.metallic_roughness_texture_deferred);
    REQUIRE(transformed.non_base_color_texture_transform_applied);
    REQUIRE(transformed.non_base_color_texcoord1_used);
    REQUIRE_FALSE(transformed.non_base_color_texture_transform_deferred);
    REQUIRE_FALSE(transformed.non_base_color_texcoord1_deferred);
    REQUIRE(transformed.texture_decoded);
    REQUIRE(transformed.fallback_texture_used);
    const HeadlessSurface::Rgba untransformed_rgba{
        untransformed.rgba,
        untransformed.width,
        untransformed.height,
    };
    const HeadlessSurface::Rgba transformed_rgba{
        transformed.rgba,
        transformed.width,
        transformed.height,
    };
    const uint64_t untransformed_fingerprint =
        HeadlessSurface::rgba_fingerprint(untransformed_rgba);
    const uint64_t transformed_fingerprint =
        HeadlessSurface::rgba_fingerprint(transformed_rgba);
    REQUIRE(untransformed_fingerprint != transformed_fingerprint);
    const double untransformed_luma =
        average_image_luma(untransformed.rgba, config.width, config.height);
    const double transformed_luma =
        average_image_luma(transformed.rgba, config.width, config.height);
    REQUIRE(untransformed_luma > transformed_luma + 4.0);
}

TEST_CASE("Renderer3D samples occlusion material texture",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto plain = Renderer3D::render_scene_data(
        make_metallic_roughness_factor_render_scene(0.0f, 0.1f),
        config);
    if (!plain.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " <<
                plain.error);
    }
    auto occluded = Renderer3D::render_scene_data(
        make_occlusion_texture_render_scene(),
        config);

    INFO(plain.error);
    INFO(occluded.error);
    REQUIRE(plain.success);
    REQUIRE(occluded.success);
    REQUIRE_FALSE(plain.occlusion_texture_applied);
    REQUIRE(occluded.occlusion_texture_applied);
    REQUIRE_FALSE(occluded.occlusion_strength_applied);
    REQUIRE_FALSE(occluded.occlusion_texture_deferred);
    REQUIRE_FALSE(occluded.occlusion_strength_deferred);
    REQUIRE(occluded.texture_decoded);
    REQUIRE(occluded.fallback_texture_used);
    const double plain_luma = average_foreground_luma(plain.rgba,
                                                      config.width,
                                                      config.height);
    const double occluded_luma = average_foreground_luma(occluded.rgba,
                                                         config.width,
                                                         config.height);
    REQUIRE(occluded_luma < plain_luma * 0.35);
}

TEST_CASE("Renderer3D applies occlusion texture strength",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto weak = Renderer3D::render_scene_data(
        make_occlusion_texture_render_scene(0.35f),
        config);
    if (!weak.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << weak.error);
    }
    auto full = Renderer3D::render_scene_data(
        make_occlusion_texture_render_scene(1.0f),
        config);

    INFO(weak.error);
    INFO(full.error);
    REQUIRE(weak.success);
    REQUIRE(full.success);
    REQUIRE(weak.occlusion_texture_applied);
    REQUIRE(full.occlusion_texture_applied);
    REQUIRE(weak.occlusion_strength_applied);
    REQUIRE_FALSE(full.occlusion_strength_applied);
    REQUIRE_FALSE(weak.occlusion_texture_deferred);
    REQUIRE_FALSE(weak.occlusion_strength_deferred);
    const double weak_luma = average_foreground_luma(weak.rgba,
                                                     config.width,
                                                     config.height);
    const double full_luma = average_foreground_luma(full.rgba,
                                                     config.width,
                                                     config.height);
    REQUIRE(full_luma < weak_luma * 0.60);
}

TEST_CASE("Renderer3D applies occlusion texture transform on TEXCOORD_1",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto untransformed = Renderer3D::render_scene_data(
        make_occlusion_texcoord1_transform_render_scene(false),
        config);
    if (!untransformed.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: "
                << untransformed.error);
    }
    auto transformed = Renderer3D::render_scene_data(
        make_occlusion_texcoord1_transform_render_scene(true),
        config);

    INFO(untransformed.error);
    INFO(transformed.error);
    REQUIRE(untransformed.success);
    REQUIRE(transformed.success);
    REQUIRE(untransformed.occlusion_texture_applied);
    REQUIRE(transformed.occlusion_texture_applied);
    REQUIRE_FALSE(transformed.occlusion_texture_deferred);
    REQUIRE_FALSE(transformed.occlusion_strength_deferred);
    REQUIRE(transformed.non_base_color_texture_transform_applied);
    REQUIRE(transformed.non_base_color_texcoord1_used);
    REQUIRE_FALSE(transformed.non_base_color_texture_transform_deferred);
    REQUIRE_FALSE(transformed.non_base_color_texcoord1_deferred);
    REQUIRE(transformed.texture_decoded);
    REQUIRE(transformed.fallback_texture_used);
    const HeadlessSurface::Rgba untransformed_rgba{
        untransformed.rgba,
        untransformed.width,
        untransformed.height,
    };
    const HeadlessSurface::Rgba transformed_rgba{
        transformed.rgba,
        transformed.width,
        transformed.height,
    };
    const uint64_t untransformed_fingerprint =
        HeadlessSurface::rgba_fingerprint(untransformed_rgba);
    const uint64_t transformed_fingerprint =
        HeadlessSurface::rgba_fingerprint(transformed_rgba);
    REQUIRE(untransformed_fingerprint != transformed_fingerprint);
    const double untransformed_luma =
        average_foreground_luma(untransformed.rgba, config.width, config.height);
    const double transformed_luma =
        average_foreground_luma(transformed.rgba, config.width, config.height);
    REQUIRE(transformed_luma > untransformed_luma * 2.0);
}
