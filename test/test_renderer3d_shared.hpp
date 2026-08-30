#pragma once

#include <catch2/catch_test_macros.hpp>
#include <pulp/render/draco_decoder.hpp>
#include <pulp/scene/draco_scene_adapter.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/renderer3d.hpp>
#include <pulp/scene/bake_preflight.hpp>
#include <pulp/scene/gltf_loader.hpp>
#include <pulp/scene/scene_data.hpp>
#include <pulp/scene/sidecar.hpp>

#ifdef PULP_TEST_HAS_DRACO
#include <draco/compression/encode.h>
#include <draco/core/encoder_buffer.h>
#include <draco/core/vector_d.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::render;

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {

inline constexpr uint64_t kMacMetalHardcodedCubeFingerprint =
    15925498132503966243ULL;
inline constexpr uint64_t kMacMetalBoxTexturedFixtureFingerprint =
    5845745157752120258ULL;

bool is_mac_metal_adapter(const HardcodedCubeRenderResult& result) {
    return result.adapter_info_available &&
           result.adapter_backend_type == "Metal";
}

bool has_native_runtime_gap(const pulp::scene::BakePreflightReport& report,
                            const std::string& feature) {
    for (const auto& gap : report.native_runtime_gaps) {
        if (gap.feature == feature) {
            return true;
        }
    }
    return false;
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void append_f32(std::vector<uint8_t>& bytes, float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    append_u32(bytes, raw);
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void pad4(std::vector<uint8_t>& bytes, uint8_t fill = 0) {
    while (bytes.size() % 4u != 0u) {
        bytes.push_back(fill);
    }
}

struct ForegroundRegion {
    uint32_t pixel_count = 0;
    uint32_t min_x = std::numeric_limits<uint32_t>::max();
    uint32_t min_y = std::numeric_limits<uint32_t>::max();
    uint32_t max_x = 0;
    uint32_t max_y = 0;
};

ForegroundRegion find_foreground_region(const std::vector<uint8_t>& rgba,
                                        uint32_t width,
                                        uint32_t height) {
    ForegroundRegion region;
    if (rgba.size() < 4 || width == 0 || height == 0) {
        return region;
    }

    const uint8_t bg_r = rgba[0];
    const uint8_t bg_g = rgba[1];
    const uint8_t bg_b = rgba[2];
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            const int dr = static_cast<int>(rgba[offset + 0]) - static_cast<int>(bg_r);
            const int dg = static_cast<int>(rgba[offset + 1]) - static_cast<int>(bg_g);
            const int db = static_cast<int>(rgba[offset + 2]) - static_cast<int>(bg_b);
            const int delta = (dr < 0 ? -dr : dr) +
                              (dg < 0 ? -dg : dg) +
                              (db < 0 ? -db : db);
            if (delta <= 18) {
                continue;
            }

            ++region.pixel_count;
            region.min_x = std::min(region.min_x, x);
            region.min_y = std::min(region.min_y, y);
            region.max_x = std::max(region.max_x, x);
            region.max_y = std::max(region.max_y, y);
        }
    }
    return region;
}

uint32_t foreground_width(const ForegroundRegion& region) {
    if (region.pixel_count == 0 || region.max_x < region.min_x) {
        return 0;
    }
    return region.max_x - region.min_x + 1u;
}

double foreground_center_x(const ForegroundRegion& region) {
    if (region.pixel_count == 0 || region.max_x < region.min_x) {
        return 0.0;
    }
    return (static_cast<double>(region.min_x) +
            static_cast<double>(region.max_x)) * 0.5;
}

double average_foreground_luma(const std::vector<uint8_t>& rgba,
                               uint32_t width,
                               uint32_t height) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4u ||
        width == 0 || height == 0) {
        return 0.0;
    }

    const uint8_t bg_r = rgba[0];
    const uint8_t bg_g = rgba[1];
    const uint8_t bg_b = rgba[2];
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            const int dr = static_cast<int>(rgba[offset + 0]) - static_cast<int>(bg_r);
            const int dg = static_cast<int>(rgba[offset + 1]) - static_cast<int>(bg_g);
            const int db = static_cast<int>(rgba[offset + 2]) - static_cast<int>(bg_b);
            const int delta = (dr < 0 ? -dr : dr) +
                              (dg < 0 ? -dg : dg) +
                              (db < 0 ? -db : db);
            if (delta <= 18) {
                continue;
            }
            sum += 0.2126 * static_cast<double>(rgba[offset + 0]) +
                   0.7152 * static_cast<double>(rgba[offset + 1]) +
                   0.0722 * static_cast<double>(rgba[offset + 2]);
            ++count;
        }
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

double average_image_luma(const std::vector<uint8_t>& rgba,
                          uint32_t width,
                          uint32_t height) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4u ||
        width == 0 || height == 0) {
        return 0.0;
    }

    double sum = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            sum += 0.2126 * static_cast<double>(rgba[offset + 0]) +
                   0.7152 * static_cast<double>(rgba[offset + 1]) +
                   0.0722 * static_cast<double>(rgba[offset + 2]);
        }
    }
    return sum / static_cast<double>(width * height);
}

double average_foreground_channel(const std::vector<uint8_t>& rgba,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t channel) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4u ||
        width == 0 || height == 0 || channel > 2) {
        return 0.0;
    }

    const uint8_t bg_r = rgba[0];
    const uint8_t bg_g = rgba[1];
    const uint8_t bg_b = rgba[2];
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            const int dr = static_cast<int>(rgba[offset + 0]) - static_cast<int>(bg_r);
            const int dg = static_cast<int>(rgba[offset + 1]) - static_cast<int>(bg_g);
            const int db = static_cast<int>(rgba[offset + 2]) - static_cast<int>(bg_b);
            const int delta = (dr < 0 ? -dr : dr) +
                              (dg < 0 ? -dg : dg) +
                              (db < 0 ? -db : db);
            if (delta <= 18) {
                continue;
            }
            sum += static_cast<double>(rgba[offset + channel]);
            ++count;
        }
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

std::vector<uint8_t> make_alpha_checker_png();

std::vector<uint8_t> make_renderable_textured_glb() {
    std::vector<uint8_t> bin;
    for (float value : {
             -1.0f, -1.0f, 0.0f,
              1.0f, -1.0f, 0.0f,
              0.0f,  1.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f,
             1.0f, 0.0f,
             0.5f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);

    const std::vector<uint8_t> checker_png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0x0f, 0x01, 0x0c,
        0x0c, 0x20, 0x02, 0xc4, 0x02, 0x00, 0x80, 0x91, 0x0d, 0xf3, 0x7e, 0xc1,
        0xff, 0xde, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
        0x60, 0x82,
    };
    const uint32_t image_offset = static_cast<uint32_t>(bin.size());
    bin.insert(bin.end(), checker_png.begin(), checker_png.end());
    const uint32_t image_length = static_cast<uint32_t>(checker_png.size());
    pad4(bin);

    const std::string json =
        R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d"},"buffers":[{"byteLength":)" +
        std::to_string(bin.size()) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":60,"byteLength":6,"target":34963},{"buffer":0,"byteOffset":)" +
        std::to_string(image_offset) +
        R"(,"byteLength":)" +
        std::to_string(image_length) +
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
             -0.65f, -0.65f, 0.0f,
              0.65f, -0.65f, 0.0f,
              0.0f,   0.65f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f,
             1.0f, 0.0f,
             0.5f, 1.0f,
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
        std::to_string(image_offset) +
        R"(,"byteLength":)" +
        std::to_string(image_length) +
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
        {-0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f, 0.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f, 1.0f, 0.0f},
        {-0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f, 0.0f, 0.0f},
        { 0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f, 0.0f, 1.0f},
        {-0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f, 1.0f, 1.0f},
        {-0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f, 1.0f, 0.0f},
        { 0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f, 0.0f, 1.0f},
        {-0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f, 1.0f, 1.0f},
        {-0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f, 1.0f, 0.0f},
        {-0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f, 0.0f, 0.0f},
        { 0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f, 0.0f, 1.0f},
        { 0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f, 1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f, 1.0f, 0.0f},
        { 0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f, 0.0f, 0.0f},
        {-0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f, 0.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f, 1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f, 1.0f, 0.0f},
        {-0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f, 0.0f, 1.0f},
        { 0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f, 1.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f, 1.0f, 0.0f},
        {-0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f, 0.0f, 0.0f},
    }};
    const std::array<uint16_t, 36> indices = {{
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23,
    }};

    std::vector<uint8_t> bin;
    const uint32_t positions_offset = static_cast<uint32_t>(bin.size());
    for (const auto& vertex : vertices) {
        append_f32(bin, vertex.x);
        append_f32(bin, vertex.y);
        append_f32(bin, vertex.z);
    }
    const uint32_t positions_length =
        static_cast<uint32_t>(bin.size()) - positions_offset;

    const uint32_t normals_offset = static_cast<uint32_t>(bin.size());
    for (const auto& vertex : vertices) {
        append_f32(bin, vertex.nx);
        append_f32(bin, vertex.ny);
        append_f32(bin, vertex.nz);
    }
    const uint32_t normals_length =
        static_cast<uint32_t>(bin.size()) - normals_offset;

    const uint32_t texcoords_offset = static_cast<uint32_t>(bin.size());
    for (const auto& vertex : vertices) {
        append_f32(bin, vertex.u);
        append_f32(bin, vertex.v);
    }
    const uint32_t texcoords_length =
        static_cast<uint32_t>(bin.size()) - texcoords_offset;

    const uint32_t indices_offset = static_cast<uint32_t>(bin.size());
    for (const auto index : indices) {
        append_u16(bin, index);
    }
    const uint32_t indices_length =
        static_cast<uint32_t>(bin.size()) - indices_offset;
    pad4(bin);

    const std::vector<uint8_t> checker_png = make_alpha_checker_png();
    const uint32_t image_offset = static_cast<uint32_t>(bin.size());
    bin.insert(bin.end(), checker_png.begin(), checker_png.end());
    const uint32_t image_length = static_cast<uint32_t>(checker_png.size());
    pad4(bin);

    const std::string json =
        R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d-box-textured"},"buffers":[{"byteLength":)" +
        std::to_string(bin.size()) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":)" +
        std::to_string(positions_offset) +
        R"(,"byteLength":)" +
        std::to_string(positions_length) +
        R"(,"target":34962},{"buffer":0,"byteOffset":)" +
        std::to_string(normals_offset) +
        R"(,"byteLength":)" +
        std::to_string(normals_length) +
        R"(,"target":34962},{"buffer":0,"byteOffset":)" +
        std::to_string(texcoords_offset) +
        R"(,"byteLength":)" +
        std::to_string(texcoords_length) +
        R"(,"target":34962},{"buffer":0,"byteOffset":)" +
        std::to_string(indices_offset) +
        R"(,"byteLength":)" +
        std::to_string(indices_length) +
        R"(,"target":34963},{"buffer":0,"byteOffset":)" +
        std::to_string(image_offset) +
        R"(,"byteLength":)" +
        std::to_string(image_length) +
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

std::filesystem::path write_temp_file(const std::string& name,
                                      const std::vector<uint8_t>& bytes) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
    return path;
}

void write_file(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

void write_file(const std::filesystem::path& path,
                const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file.good());
}

std::filesystem::path scene3d_fixture_path(const std::string& relative) {
#ifdef PULP_TEST_SCENE3D_FIXTURE_DIR
    return std::filesystem::path(PULP_TEST_SCENE3D_FIXTURE_DIR) / relative;
#else
    return std::filesystem::path("test/fixtures/scene3d") / relative;
#endif
}

pulp::scene::SceneData make_child_node_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "child-material";
    material.base_color_factor[0] = 0.25f;
    material.base_color_factor[1] = 0.75f;
    material.base_color_factor[2] = 0.35f;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "child-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData root;
    root.name = "root";
    root.translation[0] = 3.0f;
    root.children.push_back(1);
    scene.nodes.push_back(std::move(root));

    pulp::scene::NodeData child;
    child.name = "child";
    child.mesh = 0;
    child.translation[1] = -2.0f;
    child.scale[0] = 0.75f;
    child.scale[1] = 1.25f;
    scene.nodes.push_back(std::move(child));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_transformed_uv_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "fallback-base";
    texture.mime_type = "image/png";
    texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    scene.textures.push_back(std::move(texture));

    pulp::scene::MaterialData material;
    material.name = "transformed-uv-material";
    material.base_color_texture = 0;
    material.base_color_texcoord = 0;
    material.base_color_transform.enabled = true;
    material.base_color_transform.offset[0] = 0.125f;
    material.base_color_transform.offset[1] = 0.25f;
    material.base_color_transform.scale[0] = 1.5f;
    material.base_color_transform.scale[1] = 0.5f;
    material.base_color_transform.rotation = 0.2f;
    material.base_color_transform.texcoord_override = 1;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.texcoord1 = {
        0.25f, 0.25f,
        0.75f, 0.25f,
        0.5f, 0.75f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "transformed-uv-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_mipmap_sampler_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "fallback-with-mipmap-sampler";
    texture.mime_type = "image/png";
    texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "mipmap-linear";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::linear;
    sampler.min_filter =
        pulp::scene::TextureSamplerData::Filter::linear_mipmap_linear;
    scene.texture_samplers.push_back(sampler);

    pulp::scene::MaterialData material;
    material.name = "mipmap-sampler-material";
    material.base_color_texture = 0;
    material.base_color_sampler = 0;
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "mipmap-sampler-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_multi_primitive_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "multi-primitive-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.45f;
    material.base_color_factor[2] = 0.20f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData left;
    left.positions = {
        -2.0f, -1.0f, 0.0f,
        -0.4f, -1.0f, 0.0f,
        -1.2f,  1.0f, 0.0f,
    };
    left.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    left.indices = {0, 1, 2};
    left.material = 0;

    pulp::scene::PrimitiveData right;
    right.positions = {
         0.4f, -1.0f, 0.0f,
         2.0f, -1.0f, 0.0f,
         1.2f,  1.0f, 0.0f,
    };
    right.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    right.indices = {0, 1, 2};
    right.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "two-triangle-mesh";
    mesh.primitives.push_back(std::move(left));
    mesh.primitives.push_back(std::move(right));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_mixed_material_primitive_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData red;
    red.name = "red-material";
    red.base_color_factor[0] = 1.0f;
    red.base_color_factor[1] = 0.05f;
    red.base_color_factor[2] = 0.05f;
    red.double_sided = true;
    scene.materials.push_back(red);

    pulp::scene::MaterialData green;
    green.name = "green-material";
    green.base_color_factor[0] = 0.05f;
    green.base_color_factor[1] = 1.0f;
    green.base_color_factor[2] = 0.05f;
    green.double_sided = true;
    scene.materials.push_back(green);

    pulp::scene::PrimitiveData left;
    left.positions = {
        -2.0f, -1.0f, 0.0f,
        -0.4f, -1.0f, 0.0f,
        -1.2f,  1.0f, 0.0f,
    };
    left.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    left.indices = {0, 1, 2};
    left.material = 0;

    pulp::scene::PrimitiveData right;
    right.positions = {
         0.4f, -1.0f, 0.0f,
         2.0f, -1.0f, 0.0f,
         1.2f,  1.0f, 0.0f,
    };
    right.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    right.indices = {0, 1, 2};
    right.material = 1;

    pulp::scene::MeshData mesh;
    mesh.name = "mixed-material-mesh";
    mesh.primitives.push_back(std::move(left));
    mesh.primitives.push_back(std::move(right));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_directional_light_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "white-lit-material";
    material.base_color_factor[0] = 0.9f;
    material.base_color_factor[1] = 0.9f;
    material.base_color_factor[2] = 0.9f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::LightData light;
    light.name = "green-key";
    light.type = pulp::scene::LightData::Type::directional;
    light.color[0] = 0.05f;
    light.color[1] = 1.0f;
    light.color[2] = 0.05f;
    light.intensity = 1.0f;
    scene.lights.push_back(light);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "directional-light-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::NodeData light_node;
    light_node.name = "light";
    light_node.light = 0;
    scene.nodes.push_back(std::move(light_node));
    scene.root_nodes.push_back(1);

    return scene;
}

pulp::scene::SceneData make_directional_light_direction_render_scene(
    bool rotated_away) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "directional-light-direction-material";
    material.base_color_factor[0] = 0.9f;
    material.base_color_factor[1] = 0.9f;
    material.base_color_factor[2] = 0.9f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::LightData light;
    light.name = "white-key";
    light.type = pulp::scene::LightData::Type::directional;
    light.color[0] = 1.0f;
    light.color[1] = 1.0f;
    light.color[2] = 1.0f;
    light.intensity = 1.0f;
    scene.lights.push_back(light);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -0.65f, -0.65f, 0.0f,
         0.65f, -0.65f, 0.0f,
         0.0f,   0.65f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "directional-light-direction-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::NodeData light_node;
    light_node.name = "directional-light";
    light_node.light = 0;
    if (rotated_away) {
        light_node.rotation[1] = 1.0f;
        light_node.rotation[3] = 0.0f;
    }
    scene.nodes.push_back(std::move(light_node));
    scene.root_nodes.push_back(1);

    return scene;
}

pulp::scene::SceneData make_deferred_punctual_light_render_scene(
    bool attach_point_light = true,
    bool attach_spot_light = true) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "punctual-light-deferred-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::LightData point_light;
    point_light.name = "deferred-point";
    point_light.type = pulp::scene::LightData::Type::point;
    point_light.color[0] = 1.0f;
    point_light.color[1] = 0.2f;
    point_light.color[2] = 0.2f;
    point_light.intensity = 2.0f;
    point_light.range = 6.0f;
    scene.lights.push_back(point_light);

    pulp::scene::LightData spot_light;
    spot_light.name = "deferred-spot";
    spot_light.type = pulp::scene::LightData::Type::spot;
    spot_light.color[0] = 0.2f;
    spot_light.color[1] = 0.2f;
    spot_light.color[2] = 1.0f;
    spot_light.intensity = 3.0f;
    spot_light.range = 8.0f;
    spot_light.inner_cone_angle = 0.2f;
    spot_light.outer_cone_angle = 0.7f;
    scene.lights.push_back(spot_light);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "punctual-light-deferred-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    if (attach_point_light) {
        pulp::scene::NodeData point_node;
        point_node.name = "point-light";
        point_node.light = 0;
        point_node.translation[0] = 1.0f;
        point_node.translation[2] = 2.0f;
        scene.nodes.push_back(std::move(point_node));
        scene.root_nodes.push_back(
            static_cast<uint32_t>(scene.nodes.size() - 1u));
    }

    if (attach_spot_light) {
        pulp::scene::NodeData spot_node;
        spot_node.name = "spot-light";
        spot_node.light = 1;
        spot_node.translation[0] = -1.0f;
        spot_node.translation[2] = 2.0f;
        scene.nodes.push_back(std::move(spot_node));
        scene.root_nodes.push_back(
            static_cast<uint32_t>(scene.nodes.size() - 1u));
    }

    return scene;
}

pulp::scene::SceneData make_point_light_range_render_scene(float range) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "point-range-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "point-range-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::LightData point_light;
    point_light.name = "point-range";
    point_light.type = pulp::scene::LightData::Type::point;
    point_light.color[0] = 1.0f;
    point_light.color[1] = 0.25f;
    point_light.color[2] = 0.25f;
    point_light.intensity = 2.5f;
    point_light.range = range;
    scene.lights.push_back(point_light);

    pulp::scene::NodeData point_node;
    point_node.name = "point-light";
    point_node.light = 0;
    point_node.translation[2] = 2.0f;
    scene.nodes.push_back(std::move(point_node));
    scene.root_nodes.push_back(static_cast<uint32_t>(scene.nodes.size() - 1u));

    return scene;
}

pulp::scene::SceneData make_perspective_camera_render_scene(bool with_camera) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "camera-scale-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -0.65f, -0.65f, 0.0f,
         0.65f, -0.65f, 0.0f,
         0.0f,   0.65f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "camera-scale-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    if (with_camera) {
        pulp::scene::CameraData camera;
        camera.name = "perspective-camera";
        camera.projection = pulp::scene::CameraData::Projection::perspective;
        camera.yfov = 0.55f;
        camera.znear = 0.1f;
        scene.cameras.push_back(camera);

        pulp::scene::NodeData camera_node;
        camera_node.name = "camera";
        camera_node.camera = 0;
        scene.nodes.push_back(std::move(camera_node));
        scene.root_nodes.push_back(1);
    }

    return scene;
}

pulp::scene::SceneData make_transformed_camera_light_render_scene() {
    auto scene = make_perspective_camera_render_scene(true);

    pulp::scene::LightData light;
    light.name = "rotated-key";
    light.type = pulp::scene::LightData::Type::directional;
    light.color[0] = 0.8f;
    light.color[1] = 0.8f;
    light.color[2] = 1.0f;
    light.intensity = 1.2f;
    scene.lights.push_back(light);

    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.translation[2] = 4.0f;
            node.rotation[1] = 0.258819f;
            node.rotation[3] = 0.965926f;
        }
    }

    pulp::scene::NodeData light_node;
    light_node.name = "rotated-light";
    light_node.light = 0;
    light_node.rotation[0] = 0.382683f;
    light_node.rotation[3] = 0.92388f;
    scene.nodes.push_back(std::move(light_node));
    scene.root_nodes.push_back(static_cast<uint32_t>(scene.nodes.size() - 1u));

    return scene;
}

pulp::scene::SceneData make_translated_camera_render_scene(float camera_x) {
    auto scene = make_perspective_camera_render_scene(true);
    REQUIRE_FALSE(scene.meshes.empty());
    REQUIRE_FALSE(scene.meshes[0].primitives.empty());
    scene.meshes[0].primitives[0].positions = {
        -0.35f, -0.35f, 0.0f,
         0.35f, -0.35f, 0.0f,
         0.0f,   0.35f, 0.0f,
    };
    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.translation[0] = camera_x;
        }
    }
    return scene;
}

pulp::scene::SceneData make_rotated_camera_render_scene(float camera_yaw_half_sin,
                                                        float camera_yaw_half_cos) {
    auto scene = make_translated_camera_render_scene(-0.25f);
    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.rotation[1] = camera_yaw_half_sin;
            node.rotation[3] = camera_yaw_half_cos;
        }
    }
    return scene;
}

pulp::scene::SceneData make_matrix_camera_render_scene(bool rotated) {
    auto scene = make_translated_camera_render_scene(-0.25f);
    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.has_matrix_transform = true;
            for (size_t i = 0; i < 16; ++i) {
                node.matrix[i] = 0.0f;
            }
            node.matrix[0] = 1.0f;
            node.matrix[5] = 1.0f;
            node.matrix[10] = 1.0f;
            node.matrix[15] = 1.0f;
            node.matrix[12] = -0.25f;
            if (rotated) {
                constexpr float c = 0.70710677f;
                constexpr float s = 0.70710677f;
                node.matrix[0] = c;
                node.matrix[2] = -s;
                node.matrix[8] = s;
                node.matrix[10] = c;
            }
        }
    }
    return scene;
}

pulp::scene::SceneData make_camera_metadata_render_scene() {
    auto scene = make_perspective_camera_render_scene(true);
    REQUIRE_FALSE(scene.cameras.empty());
    scene.cameras[0].aspect_ratio = 1.75f;
    scene.cameras[0].znear = 0.25f;
    scene.cameras[0].zfar = 64.0f;
    return scene;
}

pulp::scene::SceneData make_camera_aspect_ratio_render_scene(float aspect_ratio) {
    auto scene = make_perspective_camera_render_scene(true);
    REQUIRE_FALSE(scene.cameras.empty());
    scene.cameras[0].aspect_ratio = aspect_ratio;
    REQUIRE_FALSE(scene.meshes.empty());
    REQUIRE_FALSE(scene.meshes[0].primitives.empty());
    scene.meshes[0].primitives[0].positions = {
        -0.45f, -0.45f, 0.0f,
         0.45f, -0.45f, 0.0f,
         0.0f,   0.45f, 0.0f,
    };
    return scene;
}

pulp::scene::SceneData make_orthographic_camera_render_scene(float ymag) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "orthographic-camera-scale-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -0.65f, -0.65f, 0.0f,
         0.65f, -0.65f, 0.0f,
         0.0f,   0.65f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "orthographic-camera-scale-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::CameraData camera;
    camera.name = "orthographic-camera";
    camera.projection = pulp::scene::CameraData::Projection::orthographic;
    camera.xmag = ymag;
    camera.ymag = ymag;
    camera.znear = 0.1f;
    camera.zfar = 10.0f;
    scene.cameras.push_back(camera);

    pulp::scene::NodeData camera_node;
    camera_node.name = "camera";
    camera_node.camera = 0;
    scene.nodes.push_back(std::move(camera_node));
    scene.root_nodes.push_back(1);

    return scene;
}

pulp::scene::SceneData make_transform_animation_render_scene(float initial_x = 0.35f) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "animated-transform-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "animated-transform-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "animated-node";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    pulp::scene::NodeData anchor_node;
    anchor_node.name = "static-anchor-node";
    anchor_node.mesh = 0;
    scene.nodes.push_back(std::move(anchor_node));
    scene.root_nodes.push_back(1);

    pulp::scene::AnimationSamplerData sampler;
    sampler.input_times = {0.0f, 1.0f};
    sampler.output_values = {
        initial_x, 0.0f, 0.0f,
        0.35f, 0.0f, 0.0f,
    };
    sampler.output_components = 3;

    pulp::scene::AnimationData animation;
    animation.name = "translation-only";
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(pulp::scene::AnimationChannelData{
        0,
        0,
        pulp::scene::AnimationChannelData::Path::translation,
    });
    scene.animations.push_back(std::move(animation));

    return scene;
}

pulp::scene::SceneData make_rotation_animation_render_scene() {
    auto scene = make_transform_animation_render_scene(0.0f);
    scene.animations.clear();

    pulp::scene::AnimationSamplerData sampler;
    sampler.input_times = {0.0f, 1.0f};
    sampler.output_components = 4;
    sampler.interpolation =
        pulp::scene::AnimationSamplerData::Interpolation::cubic_spline;
    sampler.output_values = {
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.7071068f, 0.7071068f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
    };

    pulp::scene::AnimationData animation;
    animation.name = "rotation-cubic-initial-pose";
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(pulp::scene::AnimationChannelData{
        0,
        0,
        pulp::scene::AnimationChannelData::Path::rotation,
    });
    scene.animations.push_back(std::move(animation));
    return scene;
}

pulp::scene::SceneData make_scale_animation_render_scene() {
    auto scene = make_transform_animation_render_scene(0.0f);
    scene.animations.clear();

    pulp::scene::AnimationSamplerData sampler;
    sampler.input_times = {0.0f, 1.0f};
    sampler.output_values = {
        1.45f, 0.7f, 1.0f,
        1.0f, 1.0f, 1.0f,
    };
    sampler.output_components = 3;

    pulp::scene::AnimationData animation;
    animation.name = "scale-initial-pose";
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(pulp::scene::AnimationChannelData{
        0,
        0,
        pulp::scene::AnimationChannelData::Path::scale,
    });
    scene.animations.push_back(std::move(animation));
    return scene;
}

pulp::scene::SceneData make_unsupported_feature_deferred_render_scene() {
    auto scene = make_perspective_camera_render_scene(true);
    scene.unsupported_features.push_back(pulp::scene::UnsupportedFeatureData{
        "Skinning",
        "Node references a glTF skin, but native skinning is not implemented in this slice.",
        "SkinnedNode",
    });
    scene.unsupported_features.push_back(pulp::scene::UnsupportedFeatureData{
        "MorphTargets",
        "Primitive morph targets are present, but native morph target evaluation is not implemented in this slice.",
        "MorphedMesh",
    });
    scene.unsupported_features.push_back(pulp::scene::UnsupportedFeatureData{
        "GpuInstancing",
        "EXT_mesh_gpu_instancing attributes are present, but native instanced rendering is not implemented in this slice.",
        "InstancedNode",
    });
    return scene;
}

pulp::scene::SceneData make_advanced_material_extension_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "physical-extension-material";
    material.base_color_factor[0] = 0.75f;
    material.base_color_factor[1] = 0.82f;
    material.base_color_factor[2] = 0.92f;
    material.double_sided = true;
    material.advanced_material_extensions.push_back("KHR_materials_clearcoat");
    material.advanced_material_extensions.push_back("KHR_materials_transmission");
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "physical-extension-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "physical-extension-node";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_unlit_render_scene(bool unlit) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = unlit ? "unlit-material" : "lit-material";
    material.unlit = unlit;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "unlit-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

std::vector<uint8_t> make_alpha_checker_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0x0f, 0x01, 0x0c,
        0x0c, 0x20, 0x02, 0xc4, 0x02, 0x00, 0x80, 0x91, 0x0d, 0xf3, 0x7e, 0xc1,
        0xff, 0xde, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
        0x60, 0x82,
    };
}

std::vector<uint8_t> make_blue_pixel_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0xf8, 0xff,
        0x1f, 0x00, 0x03, 0x02, 0x01, 0xff, 0xe6, 0x77, 0x0b, 0xae, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_black_pixel_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0xf8,
        0x0f, 0x00, 0x01, 0x04, 0x01, 0x00, 0x5f, 0xe5, 0xc3, 0x4b, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_black_blue_columns_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x60, 0x60, 0xf8,
        0x0f, 0x44, 0x40, 0x0c, 0x65, 0x00, 0x00, 0x2a, 0xe7, 0x05, 0xfb, 0x23,
        0x1f, 0xcb, 0xf2, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_black_white_columns_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x60, 0x60, 0xf8,
        0x0f, 0x02, 0x0c, 0x30, 0x06, 0x00, 0x4a, 0xc7, 0x09, 0xf7, 0x3d, 0x02,
        0x94, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
        0x60, 0x82,
    };
}

std::vector<uint8_t> make_reverse_z_normal_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x68, 0x68, 0x60, 0xf8,
        0x0f, 0x00, 0x04, 0x84, 0x02, 0x00, 0x0a, 0x2c, 0x99, 0xd6, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_tilted_normal_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0x9f, 0x70, 0xe0,
        0x3f, 0x00, 0x07, 0xa0, 0x03, 0x1f, 0x73, 0x5c, 0x34, 0x35, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_external_renderable_gltf_bin() {
    std::vector<uint8_t> bin;
    for (float value : {
             -1.0f, -1.0f, 0.0f,
              1.0f, -1.0f, 0.0f,
              0.0f,  1.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f,
             1.0f, 0.0f,
             0.5f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);
    return bin;
}

std::string make_external_renderable_gltf_json(size_t bin_byte_length) {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d-external"},"buffers":[{"uri":"mesh.bin","byteLength":)" +
        std::to_string(bin_byte_length) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":72,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"images":[{"uri":"checker.png","mimeType":"image/png","name":"external-checker"}],"samplers":[{"name":"externalLinearClamp","magFilter":9729,"minFilter":9729,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0,"name":"externalCheckerTexture"}],"materials":[{"name":"external-mat","pbrMetallicRoughness":{"baseColorFactor":[0.8,0.5,0.25,1],"baseColorTexture":{"index":0}},"doubleSided":true}],"meshes":[{"name":"external-triangle","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0,"mode":4}]}],"nodes":[{"name":"external-root","mesh":0,"translation":[-1,0,0]}],"scenes":[{"nodes":[0]}],"scene":0})";
}

pulp::scene::SceneData make_alpha_mask_render_scene(bool masked) {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "alpha-checker";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_alpha_checker_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::MaterialData material;
    material.name = masked ? "masked-material" : "opaque-material";
    material.base_color_texture = 0;
    if (masked) {
        material.alpha_mode = pulp::scene::MaterialData::AlphaMode::mask;
        material.alpha_cutoff = 0.5f;
    }
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-mask-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_vertex_color_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "vertex-color-material";
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.color0 = {
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "vertex-color-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_back_facing_render_scene(bool double_sided) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = double_sided ? "double-sided-material" : "single-sided-material";
    material.double_sided = double_sided;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        0.5f, 1.0f,
        1.0f, 0.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "back-facing-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_alpha_blend_render_scene(bool blended) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = blended ? "blend-material" : "opaque-material";
    material.base_color_factor[0] = 1.0f;
    material.base_color_factor[1] = 1.0f;
    material.base_color_factor[2] = 1.0f;
    material.base_color_factor[3] = blended ? 0.35f : 1.0f;
    if (blended) {
        material.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    }
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-blend-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_alpha_blend_over_opaque_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData red_blend;
    red_blend.name = "red-blend-front";
    red_blend.base_color_factor[0] = 1.0f;
    red_blend.base_color_factor[1] = 0.05f;
    red_blend.base_color_factor[2] = 0.05f;
    red_blend.base_color_factor[3] = 0.45f;
    red_blend.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    red_blend.double_sided = true;
    scene.materials.push_back(red_blend);

    pulp::scene::MaterialData green_opaque;
    green_opaque.name = "green-opaque-back";
    green_opaque.base_color_factor[0] = 0.05f;
    green_opaque.base_color_factor[1] = 1.0f;
    green_opaque.base_color_factor[2] = 0.05f;
    green_opaque.double_sided = true;
    scene.materials.push_back(green_opaque);

    pulp::scene::PrimitiveData blended_front;
    blended_front.positions = {
        -1.0f, -1.0f, 0.35f,
         1.0f, -1.0f, 0.35f,
         0.0f,  1.0f, 0.35f,
    };
    blended_front.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    blended_front.indices = {0, 1, 2};
    blended_front.material = 0;

    pulp::scene::PrimitiveData opaque_back;
    opaque_back.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    opaque_back.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    opaque_back.indices = {0, 1, 2};
    opaque_back.material = 1;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-blend-over-opaque";
    mesh.primitives.push_back(std::move(blended_front));
    mesh.primitives.push_back(std::move(opaque_back));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_alpha_blend_sorted_layers_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData red_front;
    red_front.name = "red-blend-front";
    red_front.base_color_factor[0] = 1.0f;
    red_front.base_color_factor[1] = 0.05f;
    red_front.base_color_factor[2] = 0.05f;
    red_front.base_color_factor[3] = 0.55f;
    red_front.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    red_front.double_sided = true;
    scene.materials.push_back(red_front);

    pulp::scene::MaterialData blue_back;
    blue_back.name = "blue-blend-back";
    blue_back.base_color_factor[0] = 0.05f;
    blue_back.base_color_factor[1] = 0.05f;
    blue_back.base_color_factor[2] = 1.0f;
    blue_back.base_color_factor[3] = 0.55f;
    blue_back.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    blue_back.double_sided = true;
    scene.materials.push_back(blue_back);

    pulp::scene::PrimitiveData front_submitted_first;
    front_submitted_first.positions = {
        -1.0f, -1.0f,  0.35f,
         1.0f, -1.0f,  0.35f,
         0.0f,  1.0f,  0.35f,
    };
    front_submitted_first.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    front_submitted_first.indices = {0, 1, 2};
    front_submitted_first.material = 0;

    pulp::scene::PrimitiveData back_submitted_second;
    back_submitted_second.positions = {
        -1.0f, -1.0f, -0.35f,
         1.0f, -1.0f, -0.35f,
         0.0f,  1.0f, -0.35f,
    };
    back_submitted_second.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    back_submitted_second.indices = {0, 1, 2};
    back_submitted_second.material = 1;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-blend-sorted-layers";
    mesh.primitives.push_back(std::move(front_submitted_first));
    mesh.primitives.push_back(std::move(back_submitted_second));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_emissive_render_scene(bool emissive,
                                                  float emissive_strength = 1.0f) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = emissive ? "emissive-material" : "plain-material";
    material.base_color_factor[0] = 0.1f;
    material.base_color_factor[1] = 0.1f;
    material.base_color_factor[2] = 0.1f;
    if (emissive) {
        material.emissive_factor[2] = 0.65f;
        material.emissive_strength = emissive_strength;
    }
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "emissive-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_emissive_texture_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "blue-emissive";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_blue_pixel_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::MaterialData material;
    material.name = "emissive-texture-material";
    material.base_color_factor[0] = 0.05f;
    material.base_color_factor[1] = 0.05f;
    material.base_color_factor[2] = 0.05f;
    material.emissive_factor[0] = 1.0f;
    material.emissive_factor[1] = 1.0f;
    material.emissive_factor[2] = 1.0f;
    material.emissive_strength = 1.0f;
    material.emissive_texture = 0;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "emissive-texture-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_emissive_texture_texcoord1_transform_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "black-blue-emissive";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_blue_columns_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "nearest-clamp";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.min_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.wrap_s = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    sampler.wrap_t = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    scene.texture_samplers.push_back(sampler);

    pulp::scene::MaterialData material;
    material.name = "emissive-texcoord1-transform-material";
    material.base_color_factor[0] = 0.02f;
    material.base_color_factor[1] = 0.02f;
    material.base_color_factor[2] = 0.02f;
    material.emissive_factor[0] = 1.0f;
    material.emissive_factor[1] = 1.0f;
    material.emissive_factor[2] = 1.0f;
    material.emissive_strength = 1.0f;
    material.emissive_texture = 0;
    material.emissive_sampler = 0;
    material.emissive_texcoord = 1;
    material.emissive_transform.enabled = true;
    material.emissive_transform.offset[0] = 0.5f;
    material.emissive_transform.offset[1] = 0.0f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.5f,
        0.0f, 0.5f,
        0.0f, 0.5f,
    };
    primitive.texcoord1 = {
        0.25f, 0.5f,
        0.25f, 0.5f,
        0.25f, 0.5f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "emissive-texcoord1-transform-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_metallic_roughness_factor_render_scene(
    float metallic,
    float roughness) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "metallic-roughness-factor-material";
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
    material.metallic_factor = metallic;
    material.roughness_factor = roughness;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "metallic-roughness-factor-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_metallic_roughness_texture_render_scene() {
    auto scene = make_metallic_roughness_factor_render_scene(1.0f, 1.0f);

    pulp::scene::TextureData texture;
    texture.name = "black-metallic-roughness";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_pixel_png();
    scene.textures.push_back(std::move(texture));

    scene.materials[0].metallic_roughness_texture = 0;
    return scene;
}

pulp::scene::SceneData make_metallic_roughness_texcoord1_transform_render_scene(
    bool transformed) {
    auto scene = make_metallic_roughness_factor_render_scene(1.0f, 1.0f);

    pulp::scene::TextureData texture;
    texture.name = "black-blue-metallic-roughness";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_blue_columns_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "nearest-clamp";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.min_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.wrap_s = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    sampler.wrap_t = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    scene.texture_samplers.push_back(sampler);

    scene.materials[0].metallic_roughness_texture = 0;
    scene.materials[0].metallic_roughness_sampler = 0;
    scene.materials[0].metallic_roughness_texcoord = 1;
    scene.materials[0].metallic_roughness_transform.enabled = true;
    scene.materials[0].metallic_roughness_transform.offset[0] =
        transformed ? 0.5f : 0.0f;

    auto& primitive = scene.meshes[0].primitives[0];
    primitive.texcoord0 = {
        0.0f, 0.5f,
        0.0f, 0.5f,
        0.0f, 0.5f,
    };
    primitive.texcoord1 = {
        0.25f, 0.5f,
        0.25f, 0.5f,
        0.25f, 0.5f,
    };

    return scene;
}

pulp::scene::SceneData make_occlusion_texture_render_scene(
    float occlusion_strength = 1.0f) {
    auto scene = make_metallic_roughness_factor_render_scene(0.0f, 0.1f);

    pulp::scene::TextureData texture;
    texture.name = "black-occlusion";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_pixel_png();
    scene.textures.push_back(std::move(texture));

    scene.materials[0].occlusion_texture = 0;
    scene.materials[0].occlusion_strength = occlusion_strength;
    return scene;
}

pulp::scene::SceneData make_occlusion_texcoord1_transform_render_scene(
    bool transformed) {
    auto scene = make_metallic_roughness_factor_render_scene(0.0f, 0.1f);

    pulp::scene::TextureData texture;
    texture.name = "black-white-occlusion";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_white_columns_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "nearest-clamp";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.min_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.wrap_s = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    sampler.wrap_t = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    scene.texture_samplers.push_back(sampler);

    scene.materials[0].occlusion_texture = 0;
    scene.materials[0].occlusion_sampler = 0;
    scene.materials[0].occlusion_texcoord = 1;
    scene.materials[0].occlusion_transform.enabled = true;
    scene.materials[0].occlusion_transform.offset[0] =
        transformed ? 0.5f : 0.0f;

    auto& primitive = scene.meshes[0].primitives[0];
    primitive.texcoord0 = {
        0.0f, 0.5f,
        0.0f, 0.5f,
        0.0f, 0.5f,
    };
    primitive.texcoord1 = {
        0.25f, 0.5f,
        0.25f, 0.5f,
        0.25f, 0.5f,
    };

    return scene;
}

pulp::scene::SceneData make_geometry_normal_render_scene(bool toward_light) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = toward_light ? "front-normal-material" : "back-normal-material";
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const float nz = toward_light ? 1.0f : -1.0f;
    primitive.normals = {
        0.0f, 0.0f, nz,
        0.0f, 0.0f, nz,
        0.0f, 0.0f, nz,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "normal-lit-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_normal_texture_deferred_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData normal_texture;
    normal_texture.name = "normal-map-placeholder";
    normal_texture.mime_type = "image/png";
    normal_texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    scene.textures.push_back(std::move(normal_texture));

    pulp::scene::MaterialData material;
    material.name = "normal-map-material";
    material.base_color_factor[0] = 0.7f;
    material.base_color_factor[1] = 0.7f;
    material.base_color_factor[2] = 0.7f;
    material.double_sided = true;
    material.normal_texture = 0;
    material.normal_scale = 0.6f;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    };
    primitive.tangents = {
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "normal-map-deferred-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_normal_texture_render_scene(
    float normal_scale = 1.0f,
    bool tilted_normal = false) {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData normal_texture;
    normal_texture.name = tilted_normal ? "tilted-normal" : "reverse-z-normal";
    normal_texture.mime_type = "image/png";
    normal_texture.encoded_bytes = tilted_normal
        ? make_tilted_normal_png()
        : make_reverse_z_normal_png();
    scene.textures.push_back(std::move(normal_texture));

    pulp::scene::MaterialData material;
    material.name = "sampled-normal-map-material";
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
    material.double_sided = true;
    material.normal_texture = 0;
    material.normal_scale = normal_scale;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    };
    primitive.tangents = {
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "sampled-normal-map-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_normal_texture_derived_tangent_render_scene() {
    auto scene = make_normal_texture_render_scene();
    REQUIRE_FALSE(scene.meshes.empty());
    REQUIRE_FALSE(scene.meshes[0].primitives.empty());
    scene.meshes[0].primitives[0].tangents.clear();
    scene.materials[0].normal_scale = 0.8f;
    return scene;
}

pulp::scene::SceneData make_pbr_texture_slots_deferred_render_scene() {
    pulp::scene::SceneData scene;

    for (const char* name : {
             "metallic-roughness-placeholder",
             "normal-placeholder",
             "occlusion-placeholder",
             "emissive-placeholder",
         }) {
        pulp::scene::TextureData texture;
        texture.name = name;
        texture.mime_type = "image/png";
        texture.encoded_bytes = make_black_pixel_png();
        scene.textures.push_back(std::move(texture));
    }

    pulp::scene::MaterialData material;
    material.name = "deferred-pbr-texture-slots-material";
    material.base_color_factor[0] = 0.75f;
    material.base_color_factor[1] = 0.75f;
    material.base_color_factor[2] = 0.75f;
    material.double_sided = true;
    material.metallic_roughness_texture = 0;
    material.normal_texture = 1;
    material.occlusion_texture = 2;
    material.emissive_texture = 3;
    material.metallic_roughness_texcoord = 1;
    material.normal_texcoord = 1;
    material.occlusion_texcoord = 1;
    material.emissive_texcoord = 1;
    material.metallic_roughness_transform.enabled = true;
    material.metallic_roughness_transform.offset[0] = 0.2f;
    material.metallic_roughness_transform.scale[0] = 0.75f;
    material.normal_transform.enabled = true;
    material.normal_transform.rotation = 0.1f;
    material.occlusion_transform.enabled = true;
    material.occlusion_transform.offset[1] = 0.25f;
    material.emissive_transform.enabled = true;
    material.emissive_transform.scale[1] = 1.25f;
    material.normal_scale = 0.6f;
    material.occlusion_strength = 0.4f;
    material.emissive_factor[0] = 0.1f;
    material.emissive_factor[1] = 0.05f;
    material.emissive_factor[2] = 0.0f;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.texcoord1 = {
        0.5f, 0.5f,
        0.5f, 0.5f,
        0.5f, 0.5f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "deferred-pbr-texture-slots-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

} // namespace

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
