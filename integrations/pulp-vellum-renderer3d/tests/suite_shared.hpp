#pragma once

#include <catch2/catch_test_macros.hpp>
#include <pulp/scene/bake_preflight.hpp>
#include <pulp/scene/gltf_loader.hpp>
#include <pulp/scene/scene_data.hpp>
#include <pulp/scene/sidecar.hpp>
#include <pulp/vellum_renderer3d/renderer3d.hpp>

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

using namespace pulp::vellum_renderer3d;

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {

// The legacy suites read frames through pulp::render::HeadlessSurface; this
// keeps their bodies unchanged over the package's own fingerprint.
struct HeadlessSurface {
    struct Rgba {
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    static uint64_t rgba_fingerprint(const Rgba& rgba) {
        Scene3DRenderResult frame;
        frame.width = rgba.width;
        frame.height = rgba.height;
        frame.rgba = rgba.pixels;
        return pulp::vellum_renderer3d::rgba_fingerprint(frame);
    }
};

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

ForegroundRegion find_foreground_region(const std::vector<uint8_t>& rgba, uint32_t width,
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
            const int delta = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
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

double average_foreground_luma(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4u || width == 0 || height == 0) {
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
            const int delta = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
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

double average_foreground_channel(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                                  uint32_t channel) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4u || width == 0 || height == 0 ||
        channel > 2) {
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
            const int delta = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
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

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file.good());
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
        -0.65f, -0.65f, 0.0f, 0.65f, -0.65f, 0.0f, 0.0f, 0.65f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
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

std::vector<uint8_t> make_alpha_checker_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
        0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
        0x0f, 0x01, 0x0c, 0x0c, 0x20, 0x02, 0xc4, 0x02, 0x00, 0x80, 0x91, 0x0d, 0xf3, 0x7e, 0xc1,
        0xff, 0xde, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}
} // namespace

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
