#include "test_renderer3d_shared.hpp"

TEST_CASE("Renderer3D hardcoded textured cube renders offscreen", "[render][scene3d][gpu]") {
    HardcodedCubeRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_hardcoded_textured_cube(config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
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
    REQUIRE_FALSE(result.adapter_backend_type.empty());
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

    const HeadlessSurface::Rgba rgba{
        result.rgba,
        result.width,
        result.height,
    };
    if (is_mac_metal_adapter(result)) {
        REQUIRE(HeadlessSurface::rgba_fingerprint(rgba) ==
                kMacMetalHardcodedCubeFingerprint);
    } else {
        SUCCEED("Renderer fingerprint golden is scoped to macOS Metal; adapter was "
                << result.adapter_backend_type << " / " << result.adapter_name);
    }

    auto out = std::filesystem::temp_directory_path() /
        "pulp-renderer3d-hardcoded-cube.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());
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
        SKIP("Dawn fallback adapter unavailable in this environment: "
                << result.error);
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

TEST_CASE("GpuSurface can request the Dawn null backend for API-only probes",
          "[render][scene3d][gpu][adapter]") {
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) {
        SKIP("Dawn/WebGPU unavailable in this environment");
    }

    GpuSurface::Config config;
    config.width = 16;
    config.height = 16;
    config.native_surface_handle = nullptr;
    config.backend_preference =
        GpuSurface::AdapterBackendPreference::null_backend;

    if (!gpu->initialize(config)) {
        SKIP("Dawn null backend unavailable in this environment");
    }

    const auto info = gpu->adapter_info();
    REQUIRE(info.available);
    REQUIRE(info.backend_type == "Null");
    REQUIRE(gpu->is_initialized());
    REQUIRE_FALSE(gpu->has_surface());
    REQUIRE(gpu->width() == config.width);
    REQUIRE(gpu->height() == config.height);
}

TEST_CASE("Renderer3D can request Dawn null backend for API-only probes",
          "[render][scene3d][gpu][adapter]") {
    HardcodedCubeRenderConfig config;
    config.width = 32;
    config.height = 32;
    config.backend_preference =
        Renderer3DAdapterBackendPreference::null_backend;

    auto result = Renderer3D::render_hardcoded_textured_cube(config);
    REQUIRE(result.null_backend_requested);
    if (!result.gpu_available) {
        SKIP("Dawn null backend unavailable in this environment: "
                << result.error);
    }

    INFO(result.error);
    INFO("adapter_backend_type=" << result.adapter_backend_type);
    INFO("adapter_name=" << result.adapter_name);
    REQUIRE(result.adapter_info_available);
    REQUIRE(result.adapter_backend_type == "Null");
    REQUIRE(result.width == config.width);
    REQUIRE(result.height == config.height);
    REQUIRE_FALSE(result.fallback_adapter_requested);
}

TEST_CASE("Renderer3D renders parsed SceneData offscreen", "[render][scene3d][gpu]") {
    auto path = write_temp_file("pulp-renderer3d-scenedata.glb",
                                make_renderable_textured_glb());
    auto loaded = pulp::scene::load_gltf_scene(path);
    INFO(loaded.error);
    REQUIRE(loaded.success);

    SceneDataRenderConfig config;
    config.width = 128;
    config.height = 128;

    auto result = Renderer3D::render_scene_data(loaded.scene, config);
    if (!result.gpu_available) {
        SKIP("Dawn/WebGPU unavailable in this environment: " << result.error);
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

    auto out = std::filesystem::temp_directory_path() /
        "pulp-renderer3d-scenedata.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());
}

TEST_CASE("Renderer3D renders generated textured cube GLB",
          "[render][scene3d][gpu][gltf]") {
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

    auto out = std::filesystem::temp_directory_path() /
        "pulp-renderer3d-box-textured-like.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());
}

TEST_CASE("Renderer3D renders multiple parsed GLB mesh nodes",
          "[render][scene3d][gpu][gltf]") {
    auto path = write_temp_file("pulp-renderer3d-multi-node.glb",
                                make_multi_node_textured_glb());
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

    auto foreground = find_foreground_region(result.rgba,
                                             config.width,
                                             config.height);
    REQUIRE(foreground.pixel_count > 1000);
    REQUIRE(foreground.min_x > 2);
    REQUIRE(foreground.max_x < config.width - 2);
    REQUIRE(foreground_width(foreground) > 84);

    auto out = std::filesystem::temp_directory_path() /
        "pulp-renderer3d-multi-node.png";
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
    REQUIRE(result.rgba.size() == static_cast<size_t>(config.width) *
        config.height * 4u);
    REQUIRE(result.distinct_color_count > 8);
    auto foreground = find_foreground_region(result.rgba, config.width, config.height);
    REQUIRE(foreground.pixel_count > 1500);
    REQUIRE(foreground.min_x > 4);
    REQUIRE(foreground.max_x < config.width - 4);
    REQUIRE(foreground.min_y > 0);
    REQUIRE(foreground.max_y < config.height);
    REQUIRE_FALSE(result.png.empty());

    const HeadlessSurface::Rgba rgba{
        result.rgba,
        result.width,
        result.height,
    };
    if (is_mac_metal_adapter(result)) {
        REQUIRE(HeadlessSurface::rgba_fingerprint(rgba) ==
                kMacMetalBoxTexturedFixtureFingerprint);
    } else {
        SUCCEED("Renderer fingerprint golden is scoped to macOS Metal; adapter was "
                << result.adapter_backend_type << " / " << result.adapter_name);
    }

    auto out = std::filesystem::temp_directory_path() /
        "pulp-renderer3d-box-textured-official.png";
    std::ofstream png(out, std::ios::binary);
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    REQUIRE(png.good());
}

TEST_CASE("DRACO decoder unique-id overload rejects invalid data",
          "[render][draco][scene3d]") {
    DracoAttributeIds ids;
    ids.position = 7;
    ids.normal = 8;
    ids.texcoord0 = 9;
    ids.texcoord1 = 10;
    ids.tangent = 11;
    ids.color0 = 12;

    auto empty = decode_draco(nullptr, 0, ids);
    REQUIRE_FALSE(empty.success);
    REQUIRE_FALSE(empty.unique_id_attributes_applied);

    uint8_t garbage[] = {0, 1, 2, 3};
    auto invalid = decode_draco(garbage, 4, ids);
    REQUIRE_FALSE(invalid.success);
    REQUIRE_FALSE(invalid.unique_id_attributes_applied);
}

TEST_CASE("DRACO scene adapter reaches loader callback boundary",
          "[render][draco][scene3d]") {
    auto callback = make_scene_draco_decode_callback();
    pulp::scene::DracoAttributeIds ids;
    ids.position = 7;
    ids.normal = 8;
    ids.texcoord0 = 9;
    ids.texcoord1 = 10;
    ids.tangent = 11;
    ids.color0 = 12;

    uint8_t garbage[] = {0, 1, 2, 3};
    auto decoded = callback(garbage, 4, ids);
    REQUIRE_FALSE(decoded.success);
    REQUIRE(decoded.decoder_available == draco_decoder_available());
    REQUIRE(decoded.positions.empty());
    REQUIRE(decoded.indices.empty());

    const auto dir = std::filesystem::temp_directory_path() /
        "pulp-renderer3d-draco-adapter";
    std::filesystem::create_directories(dir);
    const auto bin_path = dir / "mesh.bin";
    std::vector<uint8_t> bin;
    append_f32(bin, -1.0f); append_f32(bin, -1.0f); append_f32(bin, 0.0f);
    append_f32(bin, 1.0f); append_f32(bin, -1.0f); append_f32(bin, 0.0f);
    append_f32(bin, 0.0f); append_f32(bin, 1.0f); append_f32(bin, 0.0f);
    append_u16(bin, 0); append_u16(bin, 1); append_u16(bin, 2);
    bin.push_back(0);
    bin.push_back(1);
    bin.push_back(2);
    bin.push_back(3);
    write_file(bin_path, bin);
    const auto gltf_path = dir / "draco.gltf";
    write_file(
        gltf_path,
        R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d-draco-adapter"},"extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],"buffers":[{"uri":"mesh.bin","byteLength":46}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":6,"target":34963},{"buffer":0,"byteOffset":42,"byteLength":4}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"name":"compressed","primitives":[{"attributes":{"POSITION":0},"indices":1,"mode":4,"extensions":{"KHR_draco_mesh_compression":{"bufferView":2,"attributes":{"POSITION":7,"NORMAL":8,"TEXCOORD_0":9,"TEXCOORD_1":10,"TANGENT":11,"COLOR_0":12}}}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");

    pulp::scene::LoadOptions options;
    options.draco_decode = make_scene_draco_decode_callback();
    auto result = pulp::scene::load_gltf_scene(gltf_path, options);

    REQUIRE_FALSE(result.success);
    REQUIRE(pulp::scene::has_error_diagnostics(result.scene.diagnostics));
    REQUIRE_FALSE(result.scene.diagnostics.empty());
    if (draco_decoder_available()) {
        REQUIRE(result.scene.diagnostics[0].code == "gltf.draco_decode_failed");
    } else {
        REQUIRE(result.scene.diagnostics[0].code == "gltf.draco_unavailable");
    }
}

#ifdef PULP_TEST_HAS_DRACO
TEST_CASE("DRACO decoder preserves glTF unique-id attributes from encoded mesh",
          "[render][draco][scene3d]") {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    const int position_id = builder.AddAttribute(
        draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const int normal_id = builder.AddAttribute(
        draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
    const int texcoord0_id = builder.AddAttribute(
        draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
    const int texcoord1_id = builder.AddAttribute(
        draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
    const int tangent_id = builder.AddAttribute(
        draco::GeometryAttribute::GENERIC, 4, draco::DT_FLOAT32);
    const int color_id = builder.AddAttribute(
        draco::GeometryAttribute::COLOR, 4, draco::DT_FLOAT32);

    const draco::Vector3f p0(-1.0f, -1.0f, 0.0f);
    const draco::Vector3f p1(1.0f, -1.0f, 0.0f);
    const draco::Vector3f p2(0.0f, 1.0f, 0.0f);
    const draco::Vector3f n0(0.0f, 0.0f, 1.0f);
    const draco::Vector2f uv0(0.0f, 0.0f);
    const draco::Vector2f uv1(1.0f, 0.0f);
    const draco::Vector2f uv2(0.5f, 1.0f);
    const draco::Vector2f uv10(0.25f, 0.25f);
    const draco::Vector2f uv11(0.75f, 0.25f);
    const draco::Vector2f uv12(0.5f, 0.75f);
    const std::array<float, 4> tangent0 = {1.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 4> red = {1.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 4> green = {0.0f, 1.0f, 0.0f, 1.0f};
    const std::array<float, 4> blue = {0.0f, 0.0f, 1.0f, 1.0f};

    builder.SetAttributeValuesForFace(
        position_id, draco::FaceIndex(0), p0.data(), p1.data(), p2.data());
    builder.SetAttributeValuesForFace(
        normal_id, draco::FaceIndex(0), n0.data(), n0.data(), n0.data());
    builder.SetAttributeValuesForFace(
        texcoord0_id, draco::FaceIndex(0), uv0.data(), uv1.data(), uv2.data());
    builder.SetAttributeValuesForFace(
        texcoord1_id, draco::FaceIndex(0), uv10.data(), uv11.data(), uv12.data());
    builder.SetAttributeValuesForFace(
        tangent_id, draco::FaceIndex(0),
        tangent0.data(), tangent0.data(), tangent0.data());
    builder.SetAttributeValuesForFace(
        color_id, draco::FaceIndex(0), red.data(), green.data(), blue.data());

    builder.SetAttributeUniqueId(position_id, 7);
    builder.SetAttributeUniqueId(normal_id, 8);
    builder.SetAttributeUniqueId(texcoord0_id, 9);
    builder.SetAttributeUniqueId(texcoord1_id, 10);
    builder.SetAttributeUniqueId(tangent_id, 11);
    builder.SetAttributeUniqueId(color_id, 12);

    auto mesh = builder.Finalize();
    REQUIRE(mesh != nullptr);

    draco::Encoder encoder;
    encoder.SetSpeedOptions(10, 10);
    draco::EncoderBuffer buffer;
    const auto status = encoder.EncodeMeshToBuffer(*mesh, &buffer);
    REQUIRE(status.ok());
    REQUIRE(buffer.size() > 0);

    DracoAttributeIds ids;
    ids.position = 7;
    ids.normal = 8;
    ids.texcoord0 = 9;
    ids.texcoord1 = 10;
    ids.tangent = 11;
    ids.color0 = 12;

    const auto decoded = decode_draco(
        reinterpret_cast<const uint8_t*>(buffer.data()),
        buffer.size(),
        ids);

    REQUIRE(draco_decoder_available());
    REQUIRE(decoded.success);
    REQUIRE(decoded.unique_id_attributes_applied);
    REQUIRE(decoded.vertex_count == 3);
    REQUIRE(decoded.face_count == 1);
    REQUIRE(decoded.positions ==
            std::vector<float>{-1.0f, -1.0f, 0.0f,
                                1.0f, -1.0f, 0.0f,
                                0.0f, 1.0f, 0.0f});
    REQUIRE(decoded.normals ==
            std::vector<float>{0.0f, 0.0f, 1.0f,
                                0.0f, 0.0f, 1.0f,
                                0.0f, 0.0f, 1.0f});
    REQUIRE(decoded.tex_coords ==
            std::vector<float>{0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f});
    REQUIRE(decoded.tex_coords1 ==
            std::vector<float>{0.25f, 0.25f, 0.75f, 0.25f, 0.5f, 0.75f});
    REQUIRE(decoded.tangents ==
            std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f,
                                1.0f, 0.0f, 0.0f, 1.0f,
                                1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(decoded.colors ==
            std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f,
                                0.0f, 1.0f, 0.0f, 1.0f,
                                0.0f, 0.0f, 1.0f, 1.0f});
    REQUIRE(decoded.indices == std::vector<uint32_t>{0, 1, 2});
}
#endif
