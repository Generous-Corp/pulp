#include "suite_material_builders.hpp"

TEST_CASE("Renderer3D applies transform animation initial pose and defers playback",
          "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto static_scene = make_transform_animation_render_scene(0.0f);
    static_scene.animations.clear();
    auto static_result = Renderer3D::render_scene_data(static_scene, config);
    if (!static_result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << static_result.error);
        return;
    }

    auto result = Renderer3D::render_scene_data(make_transform_animation_render_scene(), config);

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
    const auto foreground = find_foreground_region(result.rgba, config.width, config.height);
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

    auto assert_initial_pose_changes_render = [&](pulp::scene::SceneData scene) {
        auto static_scene = scene;
        static_scene.animations.clear();
        auto static_result = Renderer3D::render_scene_data(static_scene, config);
        if (!static_result.gpu_available) {
            SKIP("Dawn/WebGPU unavailable in this environment: " << static_result.error);
            return;
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
            find_foreground_region(static_result.rgba, config.width, config.height);
        const auto foreground = find_foreground_region(result.rgba, config.width, config.height);
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

    auto result =
        Renderer3D::render_scene_data(make_unsupported_feature_deferred_render_scene(), config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.skinning_deferred);
    REQUIRE(result.morph_target_deferred);
    REQUIRE(result.gpu_instancing_deferred);
    REQUIRE(result.perspective_camera_applied);
    REQUIRE(result.base_color_factor_applied);
    const auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D reports deferred advanced material extensions", "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result =
        Renderer3D::render_scene_data(make_advanced_material_extension_render_scene(), config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.advanced_material_extension_deferred);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.double_sided_material_applied);
    REQUIRE(result.fallback_texture_used);
    const auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1200);
}

TEST_CASE("Renderer3D applies unlit material shading", "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto lit = Renderer3D::render_scene_data(make_unlit_render_scene(false), config);
    if (!lit.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << lit.error);
        return;
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

TEST_CASE("Renderer3D applies alpha-mask material cutoff", "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto opaque = Renderer3D::render_scene_data(make_alpha_mask_render_scene(false), config);
    if (!opaque.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << opaque.error);
        return;
    }
    auto masked = Renderer3D::render_scene_data(make_alpha_mask_render_scene(true), config);

    INFO(opaque.error);
    INFO(masked.error);
    REQUIRE(opaque.success);
    REQUIRE(masked.success);
    REQUIRE(opaque.texture_decoded);
    REQUIRE(masked.texture_decoded);
    REQUIRE_FALSE(opaque.alpha_mask_applied);
    REQUIRE(masked.alpha_mask_applied);
    const auto opaque_foreground = find_foreground_region(opaque.rgba, config.width, config.height);
    const auto masked_foreground = find_foreground_region(masked.rgba, config.width, config.height);
    REQUIRE(masked_foreground.pixel_count < opaque_foreground.pixel_count);
}

TEST_CASE("Renderer3D applies vertex colors", "[render][scene3d][gpu]") {
    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(make_vertex_color_render_scene(), config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
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
