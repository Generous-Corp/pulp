#pragma once

#include "suite_shared.hpp"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {

inline constexpr uint64_t kMacMetalHardcodedCubeFingerprint = 15925498132503966243ULL;
inline constexpr uint64_t kMacMetalBoxTexturedFixtureFingerprint = 5845745157752120258ULL;

bool is_mac_metal_adapter(const HardcodedCubeRenderResult& result) {
    return result.adapter_info_available && result.adapter_backend_type == "Metal";
}

std::vector<uint8_t> make_renderable_textured_glb() {
    std::vector<uint8_t> bin;
    for (float value : {
             -1.0f,
             -1.0f,
             0.0f,
             1.0f,
             -1.0f,
             0.0f,
             0.0f,
             1.0f,
             0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f,
             0.0f,
             1.0f,
             0.0f,
             0.5f,
             1.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);

    const std::vector<uint8_t> checker_png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
        0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
        0x0f, 0x01, 0x0c, 0x0c, 0x20, 0x02, 0xc4, 0x02, 0x00, 0x80, 0x91, 0x0d, 0xf3, 0x7e, 0xc1,
        0xff, 0xde, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    const uint32_t image_offset = static_cast<uint32_t>(bin.size());
    bin.insert(bin.end(), checker_png.begin(), checker_png.end());
    const uint32_t image_length = static_cast<uint32_t>(checker_png.size());
    pad4(bin);

    const std::string json =
        R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d"},"buffers":[{"byteLength":)" +
        std::to_string(bin.size()) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":60,"byteLength":6,"target":34963},{"buffer":0,"byteOffset":)" +
        std::to_string(image_offset) + R"(,"byteLength":)" + std::to_string(image_length) +
        R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],"images":[{"bufferView":3,"mimeType":"image/png","name":"checker"}],"samplers":[{"name":"linearClamp","magFilter":9729,"minFilter":9729,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0,"name":"checkerTexture"}],"materials":[{"name":"mat","pbrMetallicRoughness":{"baseColorFactor":[0.9,0.35,0.15,1],"baseColorTexture":{"index":0}},"doubleSided":true}],"meshes":[{"name":"triangle","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0,"mode":4}]}],"nodes":[{"name":"root","mesh":0,"translation":[1,2,3]}],"scenes":[{"nodes":[0]}],"scene":0})";

    std::vector<uint8_t> json_bytes(json.begin(), json.end());
    pad4(json_bytes, 0x20);

    std::vector<uint8_t> glb;
    append_u32(glb, 0x46546c67u);
    append_u32(glb, 2u);
    append_u32(glb, static_cast<uint32_t>(12u + 8u + json_bytes.size() + 8u + bin.size()));
    append_u32(glb, static_cast<uint32_t>(json_bytes.size()));
    append_u32(glb, 0x4e4f534au);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    append_u32(glb, static_cast<uint32_t>(bin.size()));
    append_u32(glb, 0x004e4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> make_multi_node_textured_glb() {
    std::vector<uint8_t> bin;
    for (float value : {
             -0.65f,
             -0.65f,
             0.0f,
             0.65f,
             -0.65f,
             0.0f,
             0.0f,
             0.65f,
             0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f,
             0.0f,
             1.0f,
             0.0f,
             0.5f,
             1.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);

    const std::vector<uint8_t> checker_png = make_alpha_checker_png();
    const uint32_t image_offset = static_cast<uint32_t>(bin.size());
    bin.insert(bin.end(), checker_png.begin(), checker_png.end());
    const uint32_t image_length = static_cast<uint32_t>(checker_png.size());
    pad4(bin);

    const std::string json =
        R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d-multi-node"},"buffers":[{"byteLength":)" +
        std::to_string(bin.size()) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":60,"byteLength":6,"target":34963},{"buffer":0,"byteOffset":)" +
        std::to_string(image_offset) + R"(,"byteLength":)" + std::to_string(image_length) +
        R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.65,-0.65,0],"max":[0.65,0.65,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],"images":[{"bufferView":3,"mimeType":"image/png","name":"checker"}],"samplers":[{"name":"linearClamp","magFilter":9729,"minFilter":9729,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0,"name":"checkerTexture"}],"materials":[{"name":"multi-node-material","pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"baseColorTexture":{"index":0}},"doubleSided":true}],"meshes":[{"name":"shared-triangle","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0,"mode":4}]}],"nodes":[{"name":"left","mesh":0,"translation":[-1.05,0,0]},{"name":"right","mesh":0,"translation":[1.05,0,0]}],"scenes":[{"nodes":[0,1]}],"scene":0})";

    std::vector<uint8_t> json_bytes(json.begin(), json.end());
    pad4(json_bytes, 0x20);

    std::vector<uint8_t> glb;
    append_u32(glb, 0x46546c67u);
    append_u32(glb, 2u);
    append_u32(glb, static_cast<uint32_t>(12u + 8u + json_bytes.size() + 8u + bin.size()));
    append_u32(glb, static_cast<uint32_t>(json_bytes.size()));
    append_u32(glb, 0x4e4f534au);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    append_u32(glb, static_cast<uint32_t>(bin.size()));
    append_u32(glb, 0x004e4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> make_renderable_textured_cube_glb() {
    struct Vertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
    };

    const std::array<Vertex, 24> vertices = {{
        {-0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        {0.5f, -0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
        {0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.5f, -0.5f, -0.5f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f},
        {-0.5f, 0.5f, -0.5f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f},
        {0.5f, 0.5f, -0.5f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {-0.5f, -0.5f, 0.5f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
        {-0.5f, 0.5f, 0.5f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {-0.5f, 0.5f, -0.5f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, -0.5f, -0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
        {0.5f, 0.5f, -0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {-0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f},
        {0.5f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        {-0.5f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, -0.5f, -0.5f, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f},
        {0.5f, -0.5f, 0.5f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f},
        {-0.5f, -0.5f, 0.5f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f},
    }};
    const std::array<uint16_t, 36> indices = {{
        0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
    }};

    std::vector<uint8_t> bin;
    const uint32_t positions_offset = static_cast<uint32_t>(bin.size());
    for (const auto& vertex : vertices) {
        append_f32(bin, vertex.x);
        append_f32(bin, vertex.y);
        append_f32(bin, vertex.z);
    }
    const uint32_t positions_length = static_cast<uint32_t>(bin.size()) - positions_offset;

    const uint32_t normals_offset = static_cast<uint32_t>(bin.size());
    for (const auto& vertex : vertices) {
        append_f32(bin, vertex.nx);
        append_f32(bin, vertex.ny);
        append_f32(bin, vertex.nz);
    }
    const uint32_t normals_length = static_cast<uint32_t>(bin.size()) - normals_offset;

    const uint32_t texcoords_offset = static_cast<uint32_t>(bin.size());
    for (const auto& vertex : vertices) {
        append_f32(bin, vertex.u);
        append_f32(bin, vertex.v);
    }
    const uint32_t texcoords_length = static_cast<uint32_t>(bin.size()) - texcoords_offset;

    const uint32_t indices_offset = static_cast<uint32_t>(bin.size());
    for (const auto index : indices) {
        append_u16(bin, index);
    }
    const uint32_t indices_length = static_cast<uint32_t>(bin.size()) - indices_offset;
    pad4(bin);

    const std::vector<uint8_t> checker_png = make_alpha_checker_png();
    const uint32_t image_offset = static_cast<uint32_t>(bin.size());
    bin.insert(bin.end(), checker_png.begin(), checker_png.end());
    const uint32_t image_length = static_cast<uint32_t>(checker_png.size());
    pad4(bin);

    const std::string json =
        R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d-box-textured"},"buffers":[{"byteLength":)" +
        std::to_string(bin.size()) + R"(}],"bufferViews":[{"buffer":0,"byteOffset":)" +
        std::to_string(positions_offset) + R"(,"byteLength":)" + std::to_string(positions_length) +
        R"(,"target":34962},{"buffer":0,"byteOffset":)" + std::to_string(normals_offset) +
        R"(,"byteLength":)" + std::to_string(normals_length) +
        R"(,"target":34962},{"buffer":0,"byteOffset":)" + std::to_string(texcoords_offset) +
        R"(,"byteLength":)" + std::to_string(texcoords_length) +
        R"(,"target":34962},{"buffer":0,"byteOffset":)" + std::to_string(indices_offset) +
        R"(,"byteLength":)" + std::to_string(indices_length) +
        R"(,"target":34963},{"buffer":0,"byteOffset":)" + std::to_string(image_offset) +
        R"(,"byteLength":)" + std::to_string(image_length) +
        R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":24,"type":"VEC3","min":[-0.5,-0.5,-0.5],"max":[0.5,0.5,0.5]},{"bufferView":1,"componentType":5126,"count":24,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":24,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":36,"type":"SCALAR"}],"images":[{"bufferView":4,"mimeType":"image/png","name":"checker"}],"samplers":[{"name":"linearClamp","magFilter":9729,"minFilter":9729,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0,"name":"checkerTexture"}],"materials":[{"name":"box-textured-material","pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"baseColorTexture":{"index":0}},"doubleSided":false}],"meshes":[{"name":"BoxTexturedLike","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0,"mode":4}]}],"nodes":[{"name":"BoxTexturedLike","mesh":0,"rotation":[0.12,0.28,0,0.952]}],"scenes":[{"nodes":[0]}],"scene":0})";

    std::vector<uint8_t> json_bytes(json.begin(), json.end());
    pad4(json_bytes, 0x20);

    std::vector<uint8_t> glb;
    append_u32(glb, 0x46546c67u);
    append_u32(glb, 2u);
    append_u32(glb, static_cast<uint32_t>(12u + 8u + json_bytes.size() + 8u + bin.size()));
    append_u32(glb, static_cast<uint32_t>(json_bytes.size()));
    append_u32(glb, 0x4e4f534au);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    append_u32(glb, static_cast<uint32_t>(bin.size()));
    append_u32(glb, 0x004e4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::filesystem::path write_temp_file(const std::string& name, const std::vector<uint8_t>& bytes) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
    return path;
}

std::filesystem::path scene3d_fixture_path(const std::string& relative) {
#ifdef PULP_TEST_SCENE3D_FIXTURE_DIR
    return std::filesystem::path(PULP_TEST_SCENE3D_FIXTURE_DIR) / relative;
#else
    return std::filesystem::path("test/fixtures/scene3d") / relative;
#endif
}
} // namespace

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
