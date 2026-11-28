#include "suite_loading_builders.hpp"

TEST_CASE("Renderer3D hardcoded textured cube renders offscreen", "[render][scene3d][gpu]") {
    HardcodedCubeRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_hardcoded_textured_cube(config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.width == config.width);
    REQUIRE(result.height == config.height);
    REQUIRE(result.color_target_allocated);
    REQUIRE(result.depth_target_allocated);
    REQUIRE(result.vertex_buffer_uploaded);
    REQUIRE(result.index_buffer_uploaded);
    REQUIRE(result.uniform_buffer_uploaded);
    REQUIRE(result.texture_uploaded);
    REQUIRE(result.command_submitted);
    REQUIRE(result.readback_completed);
    REQUIRE(result.adapter_info_available);
    REQUIRE_FALSE(result.adapter_backend.empty());
    REQUIRE((!result.adapter_backend_type.empty() && !result.adapter_type.empty()));
    REQUIRE_FALSE(result.adapter_name.empty());
    REQUIRE(result.rgba.size() == static_cast<size_t>(config.width) * config.height * 4u);
    REQUIRE(result.distinct_color_count > 1);
    REQUIRE(result.non_transparent_pixel_count > 0);
    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1500);
    REQUIRE(foreground.min_x > 3);
    REQUIRE(foreground.max_x < config.width - 3);
    REQUIRE(foreground.min_y > 3);
    REQUIRE(foreground.max_y < config.height - 3);
    REQUIRE_FALSE(result.png.empty());
    REQUIRE(result.success);

    auto out = std::filesystem::temp_directory_path() / "pulp-renderer3d-hardcoded-cube.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());

    const HeadlessSurface::Rgba rgba{
        result.rgba,
        result.width,
        result.height,
    };
    if (!is_mac_metal_adapter(result)) {
        SKIP("Renderer fingerprint golden is scoped to macOS Metal; adapter was "
             << result.adapter_backend_type << " / " << result.adapter_name);
    }
    REQUIRE(HeadlessSurface::rgba_fingerprint(rgba) == kMacMetalHardcodedCubeFingerprint);
}

TEST_CASE("Renderer3D can request the Dawn fallback adapter for golden probes",
          "[render][scene3d][gpu][adapter]") {
    HardcodedCubeRenderConfig config;
    config.width = 64;
    config.height = 64;
    config.force_fallback_adapter = true;

    auto result = Renderer3D::render_hardcoded_textured_cube(config);
    REQUIRE(result.fallback_adapter_requested);
    if (!result.gpu_available) {
        SKIP("Dawn fallback adapter unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    INFO("adapter_backend_type=" << result.adapter_backend_type);
    INFO("adapter_name=" << result.adapter_name);
    REQUIRE(result.success);
    REQUIRE(result.adapter_info_available);
    REQUIRE_FALSE(result.adapter_backend.empty());
    REQUIRE_FALSE(result.adapter_backend_type.empty());
    REQUIRE_FALSE(result.adapter_name.empty());
    REQUIRE(result.width == config.width);
    REQUIRE(result.height == config.height);
    REQUIRE(result.readback_completed);
    REQUIRE(result.distinct_color_count > 1);
    REQUIRE(result.non_transparent_pixel_count > 0);
}

TEST_CASE("A Null-adapter readback is never a frame, whatever bytes it holds",
          "[render][scene3d][adapter]") {
    // Dawn's Null adapter on Linux returns readback bytes that look like a
    // picture (169 colours over a 32x32 target); on macOS they are zeros.
    // Neither is a drawn frame.
    Scene3DRenderResult result;
    result.distinct_color_count = 169;
    result.non_transparent_pixel_count = 206;
    result.adapter_backend_type = "Vulkan";
    REQUIRE(Renderer3D::readback_holds_frame(result));
    result.adapter_backend_type = "Null";
    REQUIRE_FALSE(Renderer3D::readback_holds_frame(result));
    result.adapter_backend_type = "Metal";
    result.distinct_color_count = 1;
    REQUIRE_FALSE(Renderer3D::readback_holds_frame(result));
    result.distinct_color_count = 169;
    result.non_transparent_pixel_count = 0;
    REQUIRE_FALSE(Renderer3D::readback_holds_frame(result));
}

TEST_CASE("Renderer3D can request Dawn null backend for API-only probes",
          "[render][scene3d][gpu][adapter]") {
    HardcodedCubeRenderConfig config;
    config.width = 32;
    config.height = 32;
    config.backend_preference = Renderer3DAdapterBackendPreference::null_backend;

    auto result = Renderer3D::render_hardcoded_textured_cube(config);
    REQUIRE(result.null_backend_requested);
    if (!result.gpu_available) {
        SKIP("Dawn null backend unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    INFO("adapter_backend_type=" << result.adapter_backend_type);
    INFO("adapter_name=" << result.adapter_name);
    REQUIRE(result.adapter_info_available);
    REQUIRE(result.adapter_backend_type == "Null");
    REQUIRE(result.width == config.width);
    REQUIRE(result.height == config.height);
    REQUIRE_FALSE(result.fallback_adapter_requested);
    REQUIRE_FALSE(result.success); // it submitted, but it drew nothing
}

TEST_CASE("Renderer3D renders parsed SceneData offscreen", "[render][scene3d][gpu]") {
    auto path = write_temp_file("pulp-renderer3d-scenedata.glb", make_renderable_textured_glb());
    auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);

    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(loaded.scene, config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.width == config.width);
    REQUIRE(result.height == config.height);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.color_target_allocated);
    REQUIRE(result.depth_target_allocated);
    REQUIRE(result.vertex_buffer_uploaded);
    REQUIRE(result.index_buffer_uploaded);
    REQUIRE(result.uniform_buffer_uploaded);
    REQUIRE(result.texture_uploaded);
    REQUIRE(result.texture_decoded);
    REQUIRE_FALSE(result.fallback_texture_used);
    REQUIRE(result.base_color_texture_srgb_applied);
    REQUIRE(result.texture_sampler_applied);
    REQUIRE(result.texture_sampler_clamp_s);
    REQUIRE(result.texture_sampler_clamp_t);
    REQUIRE(result.texture_sampler_linear);
    REQUIRE(result.base_color_factor_applied);
    REQUIRE(result.command_submitted);
    REQUIRE(result.readback_completed);
    REQUIRE(result.adapter_info_available);
    REQUIRE_FALSE(result.adapter_backend.empty());
    REQUIRE_FALSE(result.adapter_backend_type.empty());
    REQUIRE_FALSE(result.adapter_name.empty());
    REQUIRE(result.rgba.size() == static_cast<size_t>(config.width) * config.height * 4u);
    REQUIRE(result.distinct_color_count > 1);
    REQUIRE(result.non_transparent_pixel_count > 0);
    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1200);
    REQUIRE(foreground.min_x > 8);
    REQUIRE(foreground.max_x < config.width - 8);
    REQUIRE(foreground.min_y > 8);
    REQUIRE(foreground.max_y < config.height - 8);
    REQUIRE_FALSE(result.png.empty());
    REQUIRE(result.success);

    auto out = std::filesystem::temp_directory_path() / "pulp-renderer3d-scenedata.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());
}

TEST_CASE("Renderer3D renders generated textured cube GLB", "[render][scene3d][gpu][gltf]") {
    auto path = write_temp_file("pulp-renderer3d-box-textured-like.glb",
                                make_renderable_textured_cube_glb());
    auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);
    REQUIRE(loaded.scene.meshes.size() == 1);
    REQUIRE(loaded.scene.meshes[0].primitives.size() == 1);
    REQUIRE(loaded.scene.meshes[0].primitives[0].positions.size() == 24u * 3u);
    REQUIRE(loaded.scene.meshes[0].primitives[0].normals.size() == 24u * 3u);
    REQUIRE(loaded.scene.meshes[0].primitives[0].texcoord0.size() == 24u * 2u);
    REQUIRE(loaded.scene.meshes[0].primitives[0].indices.size() == 36u);
    REQUIRE(loaded.scene.textures.size() == 1);
    REQUIRE_FALSE(loaded.scene.textures[0].encoded_bytes.empty());

    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(loaded.scene, config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.geometry_normals_applied);
    REQUIRE(result.texture_uploaded);
    REQUIRE(result.texture_decoded);
    REQUIRE_FALSE(result.fallback_texture_used);
    REQUIRE(result.base_color_texture_srgb_applied);
    REQUIRE(result.texture_sampler_applied);
    REQUIRE(result.readback_completed);
    REQUIRE(result.rgba.size() == static_cast<size_t>(config.width) * config.height * 4u);
    REQUIRE(result.distinct_color_count > 8);
    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1500);
    REQUIRE(foreground.min_x > 4);
    REQUIRE(foreground.max_x < config.width - 4);
    REQUIRE(foreground.min_y > 0);
    REQUIRE(foreground.max_y < config.height);
    REQUIRE_FALSE(result.png.empty());

    auto out = std::filesystem::temp_directory_path() / "pulp-renderer3d-box-textured-like.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());
}

TEST_CASE("Renderer3D renders multiple parsed GLB mesh nodes", "[render][scene3d][gpu][gltf]") {
    auto path = write_temp_file("pulp-renderer3d-multi-node.glb", make_multi_node_textured_glb());
    auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);
    REQUIRE(loaded.scene.nodes.size() == 2);
    REQUIRE(loaded.scene.root_nodes.size() == 2);
    REQUIRE(loaded.scene.meshes.size() == 1);
    REQUIRE(loaded.scene.meshes[0].primitives.size() == 1);
    REQUIRE(loaded.scene.textures.size() == 1);
    REQUIRE_FALSE(loaded.scene.textures[0].encoded_bytes.empty());

    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(loaded.scene, config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 2);
    REQUIRE(result.pipeline_cache_entry_count == 1);
    REQUIRE(result.pipeline_cache_hit_count == 1);
    REQUIRE(result.texture_uploaded);
    REQUIRE(result.texture_decoded);
    REQUIRE_FALSE(result.fallback_texture_used);
    REQUIRE(result.base_color_texture_srgb_applied);
    REQUIRE(result.texture_sampler_applied);
    REQUIRE(result.command_submitted);
    REQUIRE(result.readback_completed);

    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1000);
    REQUIRE(foreground.min_x > 2);
    REQUIRE(foreground.max_x < config.width - 2);
    REQUIRE(foreground_width(foreground) > 84);

    auto out = std::filesystem::temp_directory_path() / "pulp-renderer3d-multi-node.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());
}

TEST_CASE("Renderer3D renders official BoxTextured fixture",
          "[render][scene3d][gpu][gltf][fixture]") {
    const auto path = scene3d_fixture_path("BoxTextured/BoxTextured.glb");
    REQUIRE(std::filesystem::exists(path));

    auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);
    REQUIRE(loaded.scene.meshes.size() == 1);
    REQUIRE(loaded.scene.meshes[0].primitives.size() == 1);
    REQUIRE(loaded.scene.meshes[0].primitives[0].positions.size() == 24u * 3u);
    REQUIRE(loaded.scene.meshes[0].primitives[0].indices.size() == 36u);
    REQUIRE(loaded.scene.textures.size() == 1);
    REQUIRE_FALSE(loaded.scene.textures[0].encoded_bytes.empty());

    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(loaded.scene, config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
        return;
    }

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.scene_data_consumed);
    REQUIRE(result.primitive_count == 1);
    REQUIRE(result.geometry_normals_applied);
    REQUIRE(result.texture_uploaded);
    REQUIRE(result.texture_decoded);
    REQUIRE_FALSE(result.fallback_texture_used);
    REQUIRE(result.base_color_texture_srgb_applied);
    REQUIRE(result.readback_completed);
    REQUIRE(result.adapter_info_available);
    REQUIRE_FALSE(result.adapter_backend.empty());
    REQUIRE_FALSE(result.adapter_backend_type.empty());
    REQUIRE_FALSE(result.adapter_name.empty());
    REQUIRE(result.rgba.size() == static_cast<size_t>(config.width) * config.height * 4u);
    REQUIRE(result.distinct_color_count > 8);
    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1500);
    REQUIRE(foreground.min_x > 4);
    REQUIRE(foreground.max_x < config.width - 4);
    REQUIRE(foreground.min_y > 0);
    REQUIRE(foreground.max_y < config.height);
    REQUIRE_FALSE(result.png.empty());

    auto out = std::filesystem::temp_directory_path() / "pulp-renderer3d-box-textured-official.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());

    const HeadlessSurface::Rgba rgba{
        result.rgba,
        result.width,
        result.height,
    };
    if (!is_mac_metal_adapter(result)) {
        SKIP("Renderer fingerprint golden is scoped to macOS Metal; adapter was "
             << result.adapter_backend_type << " / " << result.adapter_name);
    }
    REQUIRE(HeadlessSurface::rgba_fingerprint(rgba) == kMacMetalBoxTexturedFixtureFingerprint);
}
