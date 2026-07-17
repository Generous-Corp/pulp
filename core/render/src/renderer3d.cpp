#include <pulp/render/renderer3d.hpp>

#include <pulp/render/gpu_surface.hpp>
#include <pulp/scene/render_packet.hpp>
#include <pulp/scene/scene_data.hpp>

#if defined(PULP_HAS_SKIA) && defined(PULP_HAS_WEBGPU)
#include "renderer3d_internal.hpp"

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "webgpu/webgpu_cpp.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::render {

namespace {

#if defined(PULP_HAS_SKIA) && defined(PULP_HAS_WEBGPU)
constexpr uint32_t align_to(uint32_t value, uint32_t alignment) {
    return ((value + alignment - 1u) / alignment) * alignment;
}

bool node_has_non_identity_trs(const pulp::scene::NodeData& node) {
    constexpr float epsilon = 0.00001f;
    auto differs = [](float value, float expected) {
        return std::abs(value - expected) > epsilon;
    };
    return differs(node.translation[0], 0.0f) ||
           differs(node.translation[1], 0.0f) ||
           differs(node.translation[2], 0.0f) ||
           differs(node.rotation[0], 0.0f) ||
           differs(node.rotation[1], 0.0f) ||
           differs(node.rotation[2], 0.0f) ||
           differs(node.rotation[3], 1.0f) ||
           differs(node.scale[0], 1.0f) ||
           differs(node.scale[1], 1.0f) ||
           differs(node.scale[2], 1.0f);
}

float dot3(float ax, float ay, float az, float bx, float by, float bz) {
    return ax * bx + ay * by + az * bz;
}

float length3(float x, float y, float z) {
    return std::sqrt(dot3(x, y, z, x, y, z));
}

void cross3(float ax,
            float ay,
            float az,
            float bx,
            float by,
            float bz,
            float& out_x,
            float& out_y,
            float& out_z) {
    out_x = ay * bz - az * by;
    out_y = az * bx - ax * bz;
    out_z = ax * by - ay * bx;
}

bool matrix_is_rigid_transform(const pulp::scene::NodeData& node) {
    if (!node.has_matrix_transform) {
        return true;
    }
    constexpr float epsilon = 0.0001f;
    if (std::abs(node.matrix[3]) > epsilon ||
        std::abs(node.matrix[7]) > epsilon ||
        std::abs(node.matrix[11]) > epsilon ||
        std::abs(node.matrix[15] - 1.0f) > epsilon) {
        return false;
    }

    const float right_length =
        length3(node.matrix[0], node.matrix[1], node.matrix[2]);
    const float up_length =
        length3(node.matrix[4], node.matrix[5], node.matrix[6]);
    const float depth_length =
        length3(node.matrix[8], node.matrix[9], node.matrix[10]);
    if (std::abs(right_length - 1.0f) > epsilon ||
        std::abs(up_length - 1.0f) > epsilon ||
        std::abs(depth_length - 1.0f) > epsilon) {
        return false;
    }

    return std::abs(dot3(node.matrix[0],
                         node.matrix[1],
                         node.matrix[2],
                         node.matrix[4],
                         node.matrix[5],
                         node.matrix[6])) <= epsilon &&
           std::abs(dot3(node.matrix[0],
                         node.matrix[1],
                         node.matrix[2],
                         node.matrix[8],
                         node.matrix[9],
                         node.matrix[10])) <= epsilon &&
           std::abs(dot3(node.matrix[4],
                         node.matrix[5],
                         node.matrix[6],
                         node.matrix[8],
                         node.matrix[9],
                         node.matrix[10])) <= epsilon;
}

bool node_has_deferred_camera_transform(const pulp::scene::NodeData& node) {
    constexpr float epsilon = 0.00001f;
    return !matrix_is_rigid_transform(node) ||
           std::abs(node.scale[0] - 1.0f) > epsilon ||
           std::abs(node.scale[1] - 1.0f) > epsilon ||
           std::abs(node.scale[2] - 1.0f) > epsilon;
}

bool node_has_deferred_light_transform(const pulp::scene::NodeData& node) {
    return node_has_deferred_camera_transform(node);
}

bool has_deferred_light_node_transform(const pulp::scene::SceneData& scene) {
    for (const auto& node : scene.nodes) {
        if (pulp::scene::is_valid_scene_index(node.light,
                                              scene.lights.size()) &&
            node_has_deferred_light_transform(node)) {
            return true;
        }
    }
    return false;
}

void record_adapter_info(const GpuSurface& gpu,
                         Scene3DRenderResult& result) {
    const auto info = gpu.adapter_info();
    result.adapter_info_available = info.available;
    result.adapter_backend = info.backend;
    result.adapter_backend_type = info.backend_type;
    result.adapter_name = info.name;
    result.adapter_vendor = info.vendor;
    result.adapter_architecture = info.architecture;
}

bool has_deferred_camera_node_transform(const pulp::scene::SceneData& scene) {
    for (const auto& node : scene.nodes) {
        if (pulp::scene::is_valid_scene_index(node.camera,
                                              scene.cameras.size()) &&
            node_has_deferred_camera_transform(node)) {
            return true;
        }
    }
    return false;
}

DeferredCameraMetadata deferred_camera_metadata(
    const pulp::scene::SceneData& scene) {
    auto scan_camera = [&scene](uint32_t camera_index,
                                DeferredCameraMetadata& out) {
        if (!pulp::scene::is_valid_scene_index(camera_index,
                                               scene.cameras.size())) {
            return;
        }
        const auto& camera = scene.cameras[camera_index];
        if (camera.aspect_ratio > 0.0f) {
            out.aspect_ratio = true;
        }
        if (camera.znear > 0.0f || camera.zfar > 0.0f) {
            out.depth_range = true;
        }
    };

    DeferredCameraMetadata out;
    for (const auto& node : scene.nodes) {
        scan_camera(node.camera, out);
    }
    for (uint32_t i = 0; i < scene.cameras.size(); ++i) {
        scan_camera(i, out);
    }
    return out;
}

bool has_deferred_camera_depth_range(
    const pulp::scene::SceneData& scene,
    uint32_t consumed_camera_index) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(scene.cameras.size()); ++i) {
        if (i == consumed_camera_index) {
            continue;
        }
        const auto& camera = scene.cameras[i];
        if (camera.znear > 0.0f || camera.zfar > 0.0f) {
            return true;
        }
    }
    return false;
}

void normalize_direction(float& x, float& y, float& z) {
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length <= 0.000001f) {
        x = 0.0f;
        y = 0.0f;
        z = 1.0f;
        return;
    }
    x /= length;
    y /= length;
    z /= length;
}

pulp::scene::TransformedPoint transform_direction(
    const pulp::scene::Mat4& transform,
    float x,
    float y,
    float z);

CpuDirectionalLight first_directional_light(
    const pulp::scene::SceneData& scene,
    const std::vector<pulp::scene::TransformedNode>& transformed_nodes) {
    auto light_from_index = [&scene](uint32_t light_index)
        -> std::optional<CpuDirectionalLight> {
        if (!pulp::scene::is_valid_scene_index(light_index,
                                               scene.lights.size())) {
            return std::nullopt;
        }
        const auto& light = scene.lights[light_index];
        if (light.type != pulp::scene::LightData::Type::directional) {
            return std::nullopt;
        }

        CpuDirectionalLight out;
        const float intensity = std::max(light.intensity, 0.0f);
        out.color[0] = std::clamp(light.color[0] * intensity, 0.0f, 4.0f);
        out.color[1] = std::clamp(light.color[1] * intensity, 0.0f, 4.0f);
        out.color[2] = std::clamp(light.color[2] * intensity, 0.0f, 4.0f);
        out.applied = true;
        return out;
    };

    for (const auto& transformed : transformed_nodes) {
        if (!pulp::scene::is_valid_scene_index(transformed.node,
                                               scene.nodes.size())) {
            continue;
        }
        const auto& node = scene.nodes[transformed.node];
        if (auto light = light_from_index(node.light)) {
            auto direction = transform_direction(transformed.world_transform,
                                                 0.0f,
                                                 0.0f,
                                                 1.0f);
            light->direction[0] = direction.x;
            light->direction[1] = direction.y;
            light->direction[2] = direction.z;
            normalize_direction(light->direction[0],
                                light->direction[1],
                                light->direction[2]);
            light->transform_applied = node_has_non_identity_trs(node) ||
                                       node.has_matrix_transform;
            return *light;
        }
    }
    for (uint32_t i = 0; i < scene.lights.size(); ++i) {
        if (auto light = light_from_index(i)) {
            return *light;
        }
    }
    return {};
}

struct DeferredPunctualLights {
    bool point = false;
    bool spot = false;
    uint32_t point_range_count = 0;
    uint32_t spot_range_count = 0;
    bool spot_cone = false;
};

DeferredPunctualLights deferred_punctual_lights(
    const pulp::scene::SceneData& scene) {
    DeferredPunctualLights out;
    for (const auto& light : scene.lights) {
        if (light.type == pulp::scene::LightData::Type::point) {
            out.point = true;
            if (light.range > 0.0f) {
                ++out.point_range_count;
            }
        } else if (light.type == pulp::scene::LightData::Type::spot) {
            out.spot = true;
            if (light.range > 0.0f) {
                ++out.spot_range_count;
            }
            if (light.inner_cone_angle > 0.0f ||
                light.outer_cone_angle != 0.7853982f) {
                out.spot_cone = true;
            }
        }
    }
    return out;
}

CpuPointLight first_point_light(
    const pulp::scene::SceneData& scene,
    const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
    const SceneNormalization& normalization) {
    for (const auto& transformed : transformed_nodes) {
        if (!pulp::scene::is_valid_scene_index(transformed.node,
                                               scene.nodes.size())) {
            continue;
        }
        const auto& node = scene.nodes[transformed.node];
        if (!pulp::scene::is_valid_scene_index(node.light,
                                               scene.lights.size())) {
            continue;
        }
        const auto& light = scene.lights[node.light];
        if (light.type != pulp::scene::LightData::Type::point) {
            continue;
        }

        CpuPointLight out;
        const float intensity = std::max(light.intensity, 0.0f);
        out.color[0] = std::clamp(light.color[0] * intensity, 0.0f, 4.0f);
        out.color[1] = std::clamp(light.color[1] * intensity, 0.0f, 4.0f);
        out.color[2] = std::clamp(light.color[2] * intensity, 0.0f, 4.0f);
        out.position[0] =
            (transformed.world_transform.m[12] - normalization.center[0]) *
            normalization.scale;
        out.position[1] =
            (transformed.world_transform.m[13] - normalization.center[1]) *
            normalization.scale;
        out.position[2] =
            (transformed.world_transform.m[14] - normalization.center[2]) *
            normalization.scale;
        if (light.range > 0.0f) {
            out.range = light.range * normalization.scale;
            out.range_applied = true;
        }
        out.applied = true;
        return out;
    }
    return {};
}

CpuSpotLight first_spot_light(
    const pulp::scene::SceneData& scene,
    const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
    const SceneNormalization& normalization) {
    for (const auto& transformed : transformed_nodes) {
        if (!pulp::scene::is_valid_scene_index(transformed.node,
                                               scene.nodes.size())) {
            continue;
        }
        const auto& node = scene.nodes[transformed.node];
        if (!pulp::scene::is_valid_scene_index(node.light,
                                               scene.lights.size())) {
            continue;
        }
        const auto& light = scene.lights[node.light];
        if (light.type != pulp::scene::LightData::Type::spot) {
            continue;
        }

        CpuSpotLight out;
        const float intensity = std::max(light.intensity, 0.0f);
        out.color[0] = std::clamp(light.color[0] * intensity, 0.0f, 4.0f);
        out.color[1] = std::clamp(light.color[1] * intensity, 0.0f, 4.0f);
        out.color[2] = std::clamp(light.color[2] * intensity, 0.0f, 4.0f);
        out.position[0] =
            (transformed.world_transform.m[12] - normalization.center[0]) *
            normalization.scale;
        out.position[1] =
            (transformed.world_transform.m[13] - normalization.center[1]) *
            normalization.scale;
        out.position[2] =
            (transformed.world_transform.m[14] - normalization.center[2]) *
            normalization.scale;
        auto direction = transform_direction(transformed.world_transform,
                                             0.0f,
                                             0.0f,
                                             -1.0f);
        out.direction[0] = direction.x;
        out.direction[1] = direction.y;
        out.direction[2] = direction.z;
        normalize_direction(out.direction[0],
                            out.direction[1],
                            out.direction[2]);
        const float inner = std::clamp(light.inner_cone_angle,
                                       0.0f,
                                       light.outer_cone_angle);
        const float outer = std::clamp(light.outer_cone_angle,
                                       inner,
                                       1.5707964f);
        out.inner_cos = std::cos(inner);
        out.outer_cos = std::cos(outer);
        if (light.range > 0.0f) {
            out.range = light.range * normalization.scale;
            out.range_applied = true;
        }
        out.applied = true;
        return out;
    }
    return {};
}

CpuCameraProjection first_camera_projection(
    const pulp::scene::SceneData& scene,
    const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
    const SceneNormalization& normalization) {
    auto apply_camera_node_offset =
        [&scene, &normalization](const pulp::scene::TransformedNode& transformed,
                                 CpuCameraProjection& projection) {
            if (!pulp::scene::is_valid_scene_index(transformed.node,
                                                   scene.nodes.size())) {
                return;
            }
            const auto& node = scene.nodes[transformed.node];
            if (!pulp::scene::is_valid_scene_index(node.camera,
                                                   scene.cameras.size())) {
                return;
            }
            if (node_has_deferred_camera_transform(node)) {
                return;
            }

            const float tx = transformed.world_transform.m[12];
            const float ty = transformed.world_transform.m[13];
            const float tz = transformed.world_transform.m[14];
            projection.camera_offset[0] =
                (tx - normalization.center[0]) * normalization.scale;
            projection.camera_offset[1] =
                (ty - normalization.center[1]) * normalization.scale;
            projection.camera_offset[2] =
                (tz - normalization.center[2]) * normalization.scale;
            constexpr float epsilon = 0.00001f;
            projection.node_translation_applied =
                std::abs(projection.camera_offset[0]) > epsilon ||
                std::abs(projection.camera_offset[1]) > epsilon ||
                std::abs(projection.camera_offset[2]) > epsilon;
            projection.camera_right[0] = transformed.world_transform.m[0];
            projection.camera_right[1] = transformed.world_transform.m[1];
            projection.camera_right[2] = transformed.world_transform.m[2];
            projection.camera_up[0] = transformed.world_transform.m[4];
            projection.camera_up[1] = transformed.world_transform.m[5];
            projection.camera_up[2] = transformed.world_transform.m[6];
            projection.camera_depth[0] = transformed.world_transform.m[8];
            projection.camera_depth[1] = transformed.world_transform.m[9];
            projection.camera_depth[2] = transformed.world_transform.m[10];
            normalize_direction(projection.camera_right[0],
                                projection.camera_right[1],
                                projection.camera_right[2]);
            normalize_direction(projection.camera_up[0],
                                projection.camera_up[1],
                                projection.camera_up[2]);
            normalize_direction(projection.camera_depth[0],
                                projection.camera_depth[1],
                                projection.camera_depth[2]);
            projection.node_rotation_applied =
                std::abs(projection.camera_right[0] - 1.0f) > epsilon ||
                std::abs(projection.camera_right[1]) > epsilon ||
                std::abs(projection.camera_right[2]) > epsilon ||
                std::abs(projection.camera_up[0]) > epsilon ||
                std::abs(projection.camera_up[1] - 1.0f) > epsilon ||
                std::abs(projection.camera_up[2]) > epsilon ||
                std::abs(projection.camera_depth[0]) > epsilon ||
                std::abs(projection.camera_depth[1]) > epsilon ||
                std::abs(projection.camera_depth[2] - 1.0f) > epsilon;
        };

    auto camera_from_index = [&scene](uint32_t camera_index)
        -> std::optional<CpuCameraProjection> {
        if (!pulp::scene::is_valid_scene_index(camera_index,
                                               scene.cameras.size())) {
            return std::nullopt;
        }
        const auto& camera = scene.cameras[camera_index];
        CpuCameraProjection out;
        out.camera_index = camera_index;
        if (camera.aspect_ratio > 0.0f) {
            out.aspect_ratio = camera.aspect_ratio;
            out.aspect_ratio_applied = true;
        }
        if (camera.znear > 0.0f || camera.zfar > 0.0f) {
            out.znear = camera.znear > 0.0f ? camera.znear : 0.1f;
            out.zfar = camera.zfar > out.znear ? camera.zfar : out.znear + 100.0f;
            out.depth_range_applied = true;
        }
        if (camera.projection ==
            pulp::scene::CameraData::Projection::perspective) {
            const float yfov = camera.yfov > 0.0f ? camera.yfov : 0.9f;
            out.projection_scale = 0.6f /
                std::max(std::tan(yfov * 0.5f), 0.05f);
            out.perspective_applied = true;
            return out;
        }
        if (camera.projection ==
            pulp::scene::CameraData::Projection::orthographic) {
            const float ymag = camera.ymag > 0.0f ? camera.ymag : 2.0f;
            out.projection_scale = 0.95f / std::max(ymag, 0.05f);
            out.orthographic_applied = true;
            return out;
        }
        return std::nullopt;
    };

    for (const auto& transformed : transformed_nodes) {
        if (!pulp::scene::is_valid_scene_index(transformed.node,
                                               scene.nodes.size())) {
            continue;
        }
        const auto& node = scene.nodes[transformed.node];
        if (auto camera = camera_from_index(node.camera)) {
            apply_camera_node_offset(transformed, *camera);
            return *camera;
        }
    }
    for (uint32_t i = 0; i < scene.cameras.size(); ++i) {
        if (auto camera = camera_from_index(i)) {
            return *camera;
        }
    }
    return {};
}

bool has_transform_animation(const pulp::scene::SceneData& scene) {
    for (const auto& animation : scene.animations) {
        for (const auto& channel : animation.channels) {
            switch (channel.path) {
                case pulp::scene::AnimationChannelData::Path::translation:
                case pulp::scene::AnimationChannelData::Path::rotation:
                case pulp::scene::AnimationChannelData::Path::scale:
                    return true;
            }
        }
    }
    return false;
}

bool apply_initial_animation_pose(pulp::scene::SceneData& scene) {
    bool applied = false;
    for (const auto& animation : scene.animations) {
        for (const auto& channel : animation.channels) {
            if (!pulp::scene::is_valid_scene_index(channel.node,
                                                   scene.nodes.size()) ||
                !pulp::scene::is_valid_scene_index(channel.sampler,
                                                   animation.samplers.size())) {
                continue;
            }
            const auto& sampler = animation.samplers[channel.sampler];
            const uint32_t components = sampler.output_components;
            if (components == 0u || sampler.output_values.empty()) {
                continue;
            }
            const size_t value_offset =
                sampler.interpolation ==
                        pulp::scene::AnimationSamplerData::Interpolation::cubic_spline
                    ? components
                    : 0u;
            if (value_offset + components > sampler.output_values.size()) {
                continue;
            }

            auto& node = scene.nodes[channel.node];
            node.has_matrix_transform = false;
            const float* values = sampler.output_values.data() + value_offset;
            switch (channel.path) {
                case pulp::scene::AnimationChannelData::Path::translation:
                    if (components < 3u) {
                        continue;
                    }
                    node.translation[0] = values[0];
                    node.translation[1] = values[1];
                    node.translation[2] = values[2];
                    applied = true;
                    break;
                case pulp::scene::AnimationChannelData::Path::rotation:
                    if (components < 4u) {
                        continue;
                    }
                    node.rotation[0] = values[0];
                    node.rotation[1] = values[1];
                    node.rotation[2] = values[2];
                    node.rotation[3] = values[3];
                    applied = true;
                    break;
                case pulp::scene::AnimationChannelData::Path::scale:
                    if (components < 3u) {
                        continue;
                    }
                    node.scale[0] = values[0];
                    node.scale[1] = values[1];
                    node.scale[2] = values[2];
                    applied = true;
                    break;
            }
        }
    }
    return applied;
}

DeferredUnsupportedFeatures deferred_unsupported_features(
    const pulp::scene::SceneData& scene) {
    DeferredUnsupportedFeatures out;
    for (const auto& feature : scene.unsupported_features) {
        if (feature.feature == "Skinning") {
            out.skinning = true;
        } else if (feature.feature == "MorphTargets" ||
                   feature.feature == "MorphWeights") {
            out.morph_target = true;
        } else if (feature.feature == "GpuInstancing") {
            out.gpu_instancing = true;
        }
    }
    return out;
}

pulp::scene::TransformedPoint transform_direction(
    const pulp::scene::Mat4& transform,
    float x,
    float y,
    float z) {
    return pulp::scene::TransformedPoint{
        transform.m[0] * x + transform.m[4] * y + transform.m[8] * z,
        transform.m[1] * x + transform.m[5] * y + transform.m[9] * z,
        transform.m[2] * x + transform.m[6] * y + transform.m[10] * z,
    };
}

std::optional<CpuTexture> decode_texture_rgba(
    const pulp::scene::TextureData& texture) {
    if (texture.encoded_bytes.empty()) {
        return std::nullopt;
    }

    auto sk_data = SkData::MakeWithoutCopy(texture.encoded_bytes.data(),
                                           texture.encoded_bytes.size());
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image || image->width() <= 0 || image->height() <= 0) {
        return std::nullopt;
    }

    CpuTexture out;
    out.width = static_cast<uint32_t>(image->width());
    out.height = static_cast<uint32_t>(image->height());
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4u);
    auto info = SkImageInfo::Make(image->width(),
                                  image->height(),
                                  kRGBA_8888_SkColorType,
                                  kUnpremul_SkAlphaType);
    if (!image->readPixels(info,
                           out.rgba.data(),
                           static_cast<size_t>(out.width) * 4u,
                           0,
                           0)) {
        return std::nullopt;
    }
    out.decoded = true;
    return out;
}

uint8_t color_factor_to_byte(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

CpuTexture make_fallback_texture() {
    CpuTexture out;
    out.width = 4;
    out.height = 4;
    out.rgba.resize(4u * 4u * 4u);
    out.fallback = true;
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            const bool bright = ((x + y) % 2u) == 0u;
            const float scale = bright ? 1.0f : 0.45f;
            const size_t offset = (static_cast<size_t>(y) * out.width + x) * 4u;
            out.rgba[offset + 0] = color_factor_to_byte(scale);
            out.rgba[offset + 1] = color_factor_to_byte(scale);
            out.rgba[offset + 2] = color_factor_to_byte(scale);
            out.rgba[offset + 3] = 255;
        }
    }
    return out;
}

CpuTexture make_solid_texture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    CpuTexture out;
    out.width = 1;
    out.height = 1;
    out.rgba = {r, g, b, a};
    out.fallback = true;
    return out;
}

wgpu::AddressMode address_mode_from_scene_wrap(
    pulp::scene::TextureSamplerData::Wrap wrap) {
    switch (wrap) {
        case pulp::scene::TextureSamplerData::Wrap::clamp_to_edge:
            return wgpu::AddressMode::ClampToEdge;
        case pulp::scene::TextureSamplerData::Wrap::mirrored_repeat:
            return wgpu::AddressMode::MirrorRepeat;
        case pulp::scene::TextureSamplerData::Wrap::repeat:
            return wgpu::AddressMode::Repeat;
    }
    return wgpu::AddressMode::Repeat;
}

wgpu::FilterMode filter_mode_from_scene_filter(
    pulp::scene::TextureSamplerData::Filter filter,
    wgpu::FilterMode fallback) {
    switch (filter) {
        case pulp::scene::TextureSamplerData::Filter::nearest:
        case pulp::scene::TextureSamplerData::Filter::nearest_mipmap_nearest:
        case pulp::scene::TextureSamplerData::Filter::nearest_mipmap_linear:
            return wgpu::FilterMode::Nearest;
        case pulp::scene::TextureSamplerData::Filter::linear:
        case pulp::scene::TextureSamplerData::Filter::linear_mipmap_nearest:
        case pulp::scene::TextureSamplerData::Filter::linear_mipmap_linear:
            return wgpu::FilterMode::Linear;
        case pulp::scene::TextureSamplerData::Filter::unspecified:
            return fallback;
    }
    return fallback;
}

wgpu::MipmapFilterMode mipmap_filter_mode_from_scene_filter(
    pulp::scene::TextureSamplerData::Filter filter) {
    switch (filter) {
        case pulp::scene::TextureSamplerData::Filter::nearest_mipmap_nearest:
        case pulp::scene::TextureSamplerData::Filter::linear_mipmap_nearest:
            return wgpu::MipmapFilterMode::Nearest;
        case pulp::scene::TextureSamplerData::Filter::nearest_mipmap_linear:
        case pulp::scene::TextureSamplerData::Filter::linear_mipmap_linear:
            return wgpu::MipmapFilterMode::Linear;
        case pulp::scene::TextureSamplerData::Filter::unspecified:
        case pulp::scene::TextureSamplerData::Filter::nearest:
        case pulp::scene::TextureSamplerData::Filter::linear:
            return wgpu::MipmapFilterMode::Nearest;
    }
    return wgpu::MipmapFilterMode::Nearest;
}

bool scene_filter_requires_mipmaps(
    pulp::scene::TextureSamplerData::Filter filter) {
    switch (filter) {
        case pulp::scene::TextureSamplerData::Filter::nearest_mipmap_nearest:
        case pulp::scene::TextureSamplerData::Filter::linear_mipmap_nearest:
        case pulp::scene::TextureSamplerData::Filter::nearest_mipmap_linear:
        case pulp::scene::TextureSamplerData::Filter::linear_mipmap_linear:
            return true;
        case pulp::scene::TextureSamplerData::Filter::unspecified:
        case pulp::scene::TextureSamplerData::Filter::nearest:
        case pulp::scene::TextureSamplerData::Filter::linear:
            return false;
    }
    return false;
}

std::pair<float, float> transformed_texture_uv(
    const pulp::scene::PrimitiveData& primitive,
    uint32_t texture_texcoord,
    const pulp::scene::TextureTransformData& texture_transform,
    size_t vertex_index,
    bool* transform_applied,
    bool* texcoord1_used,
    bool* unsupported_texcoord) {
    uint32_t texcoord = texture_texcoord;
    if (texture_transform.enabled &&
        texture_transform.texcoord_override != pulp::scene::invalid_scene_index) {
        texcoord = texture_transform.texcoord_override;
    }

    float u = 0.5f;
    float v = 0.5f;
    if (texcoord == 1u && primitive.texcoord1.size() >= (vertex_index + 1u) * 2u) {
        u = primitive.texcoord1[vertex_index * 2u + 0u];
        v = primitive.texcoord1[vertex_index * 2u + 1u];
        if (texcoord1_used) {
            *texcoord1_used = true;
        }
    } else if (texcoord == 0u &&
               primitive.texcoord0.size() >= (vertex_index + 1u) * 2u) {
        u = primitive.texcoord0[vertex_index * 2u + 0u];
        v = primitive.texcoord0[vertex_index * 2u + 1u];
    } else {
        if (unsupported_texcoord) {
            *unsupported_texcoord = true;
        }
        if (primitive.texcoord0.size() >= (vertex_index + 1u) * 2u) {
            u = primitive.texcoord0[vertex_index * 2u + 0u];
            v = primitive.texcoord0[vertex_index * 2u + 1u];
        }
    }

    if (texture_transform.enabled) {
        const float scaled_u = u * texture_transform.scale[0];
        const float scaled_v = v * texture_transform.scale[1];
        const float c = std::cos(texture_transform.rotation);
        const float s = std::sin(texture_transform.rotation);
        u = scaled_u * c - scaled_v * s + texture_transform.offset[0];
        v = scaled_u * s + scaled_v * c + texture_transform.offset[1];
        if (transform_applied) {
            *transform_applied = true;
        }
    }

    return {u, v};
}

std::pair<float, float> transformed_base_color_uv(
    const pulp::scene::PrimitiveData& primitive,
    const pulp::scene::MaterialData* material,
    size_t vertex_index,
    bool* transform_applied,
    bool* texcoord1_used) {
    static const pulp::scene::TextureTransformData kDefaultTransform;
    bool unsupported = false;
    return transformed_texture_uv(
        primitive,
        material ? material->base_color_texcoord : 0u,
        material ? material->base_color_transform : kDefaultTransform,
        vertex_index,
        transform_applied,
        texcoord1_used,
        &unsupported);
}

bool texture_uv_route_supported(
    const pulp::scene::PrimitiveData& primitive,
    uint32_t texture_texcoord,
    const pulp::scene::TextureTransformData& texture_transform) {
    uint32_t texcoord = texture_texcoord;
    if (texture_transform.enabled &&
        texture_transform.texcoord_override != pulp::scene::invalid_scene_index) {
        texcoord = texture_transform.texcoord_override;
    }

    const size_t vertex_count = primitive.positions.size() / 3u;
    if (texcoord == 0u) {
        return primitive.texcoord0.size() >= vertex_count * 2u;
    }
    if (texcoord == 1u) {
        return primitive.texcoord1.size() >= vertex_count * 2u;
    }
    return false;
}

bool derive_tangents_from_positions_normals_uvs(std::vector<SceneVertex>& vertices,
                                                const std::vector<uint32_t>& indices) {
    if (vertices.empty() || indices.size() < 3u || indices.size() % 3u != 0u) {
        return false;
    }

    std::vector<std::array<float, 3>> tangent_sum(vertices.size());
    std::vector<std::array<float, 3>> bitangent_sum(vertices.size());
    bool derived_any = false;

    for (size_t index = 0; index + 2u < indices.size(); index += 3u) {
        const uint32_t i0 = indices[index + 0u];
        const uint32_t i1 = indices[index + 1u];
        const uint32_t i2 = indices[index + 2u];
        if (i0 >= vertices.size() || i1 >= vertices.size() ||
            i2 >= vertices.size()) {
            continue;
        }

        const auto& v0 = vertices[i0];
        const auto& v1 = vertices[i1];
        const auto& v2 = vertices[i2];
        const float x1 = v1.x - v0.x;
        const float y1 = v1.y - v0.y;
        const float z1 = v1.z - v0.z;
        const float x2 = v2.x - v0.x;
        const float y2 = v2.y - v0.y;
        const float z2 = v2.z - v0.z;
        const float s1 = v1.normal_u - v0.normal_u;
        const float t1 = v1.normal_v - v0.normal_v;
        const float s2 = v2.normal_u - v0.normal_u;
        const float t2 = v2.normal_v - v0.normal_v;
        const float determinant = s1 * t2 - s2 * t1;
        if (std::abs(determinant) <= 0.000001f) {
            continue;
        }

        const float r = 1.0f / determinant;
        const std::array<float, 3> tangent{
            (t2 * x1 - t1 * x2) * r,
            (t2 * y1 - t1 * y2) * r,
            (t2 * z1 - t1 * z2) * r,
        };
        const std::array<float, 3> bitangent{
            (s1 * x2 - s2 * x1) * r,
            (s1 * y2 - s2 * y1) * r,
            (s1 * z2 - s2 * z1) * r,
        };

        for (uint32_t vertex_index : {i0, i1, i2}) {
            tangent_sum[vertex_index][0] += tangent[0];
            tangent_sum[vertex_index][1] += tangent[1];
            tangent_sum[vertex_index][2] += tangent[2];
            bitangent_sum[vertex_index][0] += bitangent[0];
            bitangent_sum[vertex_index][1] += bitangent[1];
            bitangent_sum[vertex_index][2] += bitangent[2];
        }
        derived_any = true;
    }

    if (!derived_any) {
        return false;
    }

    bool all_vertices_derived = true;
    for (size_t i = 0; i < vertices.size(); ++i) {
        auto& vertex = vertices[i];
        float tx = tangent_sum[i][0];
        float ty = tangent_sum[i][1];
        float tz = tangent_sum[i][2];
        const float normal_dot_tangent =
            dot3(vertex.nx, vertex.ny, vertex.nz, tx, ty, tz);
        tx -= vertex.nx * normal_dot_tangent;
        ty -= vertex.ny * normal_dot_tangent;
        tz -= vertex.nz * normal_dot_tangent;
        const float tangent_length = length3(tx, ty, tz);
        if (tangent_length <= 0.000001f) {
            all_vertices_derived = false;
            continue;
        }
        tx /= tangent_length;
        ty /= tangent_length;
        tz /= tangent_length;

        float cross_x = 0.0f;
        float cross_y = 0.0f;
        float cross_z = 0.0f;
        cross3(vertex.nx, vertex.ny, vertex.nz, tx, ty, tz,
               cross_x, cross_y, cross_z);
        const float handedness =
            dot3(cross_x,
                 cross_y,
                 cross_z,
                 bitangent_sum[i][0],
                 bitangent_sum[i][1],
                 bitangent_sum[i][2]) < 0.0f
            ? -1.0f
            : 1.0f;

        vertex.tx = tx;
        vertex.ty = ty;
        vertex.tz = tz;
        vertex.tw = handedness;
    }

    return all_vertices_derived;
}

std::vector<CpuPrimitive> collect_renderable_primitives(
    const pulp::scene::SceneData& scene,
    SceneNormalization* normalization,
    std::vector<pulp::scene::TransformedNode>* transformed_nodes_out,
    std::string* error) {
    const auto packet = pulp::scene::build_render_packet(scene);
    if (packet.has_errors()) {
        if (error) {
            *error = "Renderer3D: SceneData render-packet build failed";
        }
        return {};
    }

    if (transformed_nodes_out != nullptr) {
        *transformed_nodes_out = packet.transformed_nodes;
    }

    std::vector<CpuPrimitive> draws;
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float min_z = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    float max_z = std::numeric_limits<float>::lowest();

    for (const auto& render_primitive : packet.primitives) {
        if (!pulp::scene::is_valid_scene_index(render_primitive.mesh,
                                               scene.meshes.size())) {
            continue;
        }
        const auto& mesh = scene.meshes[render_primitive.mesh];
        if (!pulp::scene::is_valid_scene_index(render_primitive.primitive,
                                               mesh.primitives.size())) {
            continue;
        }
        const auto& primitive = mesh.primitives[render_primitive.primitive];
        if (primitive.positions.empty() || primitive.indices.empty()) {
            continue;
        }

        const pulp::scene::MaterialData* material = nullptr;
        if (pulp::scene::is_valid_scene_index(primitive.material,
                                              scene.materials.size())) {
            material = &scene.materials[primitive.material];
        }

            const size_t vertex_count = primitive.positions.size() / 3u;
            const bool has_geometry_normals =
                primitive.normals.size() >= vertex_count * 3u;
            CpuPrimitive draw;
            draw.vertices.resize(vertex_count);
            draw.geometry_normals_applied = has_geometry_normals;
            const bool has_tangent_attributes =
                primitive.tangents.size() >= vertex_count * 4u;
            draw.tangent_attributes_available = has_tangent_attributes;
            if (material != nullptr) {
                draw.base_color_factor[0] = material->base_color_factor[0];
                draw.base_color_factor[1] = material->base_color_factor[1];
                draw.base_color_factor[2] = material->base_color_factor[2];
                draw.base_color_factor[3] = material->base_color_factor[3];
                draw.base_color_factor_applied = true;
                draw.metallic_factor =
                    std::clamp(material->metallic_factor, 0.0f, 1.0f);
                draw.roughness_factor =
                    std::clamp(material->roughness_factor, 0.0f, 1.0f);
                draw.metallic_roughness_factor_applied = true;
                draw.normal_scale =
                    std::clamp(material->normal_scale, 0.0f, 4.0f);
                draw.occlusion_strength =
                    std::clamp(material->occlusion_strength, 0.0f, 1.0f);
                draw.emissive_factor[0] =
                    material->emissive_factor[0] * material->emissive_strength;
                draw.emissive_factor[1] =
                    material->emissive_factor[1] * material->emissive_strength;
                draw.emissive_factor[2] =
                    material->emissive_factor[2] * material->emissive_strength;
                draw.emissive_factor_applied =
                    draw.emissive_factor[0] > 0.0f ||
                    draw.emissive_factor[1] > 0.0f ||
                    draw.emissive_factor[2] > 0.0f;
                draw.emissive_strength_applied =
                    material->emissive_strength != 1.0f &&
                    (material->emissive_factor[0] > 0.0f ||
                     material->emissive_factor[1] > 0.0f ||
                     material->emissive_factor[2] > 0.0f);
                draw.unlit = material->unlit;
                draw.alpha_mask =
                    material->alpha_mode ==
                    pulp::scene::MaterialData::AlphaMode::mask;
                draw.alpha_blend =
                    material->alpha_mode ==
                    pulp::scene::MaterialData::AlphaMode::blend;
                draw.alpha_cutoff = material->alpha_cutoff;
                draw.double_sided = material->double_sided;
                draw.metallic_roughness_texture_deferred =
                    pulp::scene::is_valid_scene_index(
                        material->metallic_roughness_texture,
                        scene.textures.size());
                draw.normal_texture_deferred =
                    pulp::scene::is_valid_scene_index(
                        material->normal_texture,
                        scene.textures.size());
                draw.normal_scale_deferred =
                    draw.normal_texture_deferred &&
                    material->normal_scale != 1.0f;
                draw.occlusion_texture_deferred =
                    pulp::scene::is_valid_scene_index(
                        material->occlusion_texture,
                        scene.textures.size());
                draw.occlusion_strength_deferred =
                    draw.occlusion_texture_deferred &&
                    material->occlusion_strength != 1.0f;
                draw.emissive_texture_deferred =
                    pulp::scene::is_valid_scene_index(
                        material->emissive_texture,
                        scene.textures.size());
                draw.advanced_material_extension_deferred =
                    !material->advanced_material_extensions.empty();
            }

            for (size_t i = 0; i < vertex_count; ++i) {
                const auto point = pulp::scene::transform_point(
                    render_primitive.world_transform,
                    primitive.positions[i * 3u + 0u],
                    primitive.positions[i * 3u + 1u],
                    primitive.positions[i * 3u + 2u]);
                draw.vertices[i].x = point.x;
                draw.vertices[i].y = point.y;
                draw.vertices[i].z = point.z;
                const auto uv = transformed_base_color_uv(
                    primitive,
                    material,
                    i,
                    &draw.base_color_transform_applied,
                    &draw.base_color_texcoord1_used);
                draw.vertices[i].u = uv.first;
                draw.vertices[i].v = uv.second;
                if (material != nullptr) {
                    const bool has_metallic_roughness_texture =
                        pulp::scene::is_valid_scene_index(
                            material->metallic_roughness_texture,
                            scene.textures.size());
                    bool metallic_roughness_transform_applied = false;
                    bool metallic_roughness_texcoord1_used = false;
                    bool unsupported_metallic_roughness_texcoord = false;
                    const auto metallic_roughness_uv = transformed_texture_uv(
                        primitive,
                        material->metallic_roughness_texcoord,
                        material->metallic_roughness_transform,
                        i,
                        &metallic_roughness_transform_applied,
                        &metallic_roughness_texcoord1_used,
                        &unsupported_metallic_roughness_texcoord);
                    draw.vertices[i].metallic_roughness_u =
                        metallic_roughness_uv.first;
                    draw.vertices[i].metallic_roughness_v =
                        metallic_roughness_uv.second;
                    draw.non_base_color_texture_transform_applied =
                        draw.non_base_color_texture_transform_applied ||
                        (has_metallic_roughness_texture &&
                         metallic_roughness_transform_applied);
                    draw.non_base_color_texcoord1_used =
                        draw.non_base_color_texcoord1_used ||
                        (has_metallic_roughness_texture &&
                         metallic_roughness_texcoord1_used);
                    draw.non_base_color_texcoord1_deferred =
                        draw.non_base_color_texcoord1_deferred ||
                        unsupported_metallic_roughness_texcoord;

                    const bool has_normal_texture =
                        pulp::scene::is_valid_scene_index(
                            material->normal_texture,
                            scene.textures.size());
                    bool normal_transform_applied = false;
                    bool normal_texcoord1_used = false;
                    bool unsupported_normal_texcoord = false;
                    const auto normal_uv = transformed_texture_uv(
                        primitive,
                        material->normal_texcoord,
                        material->normal_transform,
                        i,
                        &normal_transform_applied,
                        &normal_texcoord1_used,
                        &unsupported_normal_texcoord);
                    draw.vertices[i].normal_u = normal_uv.first;
                    draw.vertices[i].normal_v = normal_uv.second;
                    draw.non_base_color_texture_transform_applied =
                        draw.non_base_color_texture_transform_applied ||
                        (has_normal_texture && normal_transform_applied);
                    draw.non_base_color_texcoord1_used =
                        draw.non_base_color_texcoord1_used ||
                        (has_normal_texture && normal_texcoord1_used);
                    draw.non_base_color_texcoord1_deferred =
                        draw.non_base_color_texcoord1_deferred ||
                        unsupported_normal_texcoord;

                    const bool has_occlusion_texture =
                        pulp::scene::is_valid_scene_index(
                            material->occlusion_texture,
                            scene.textures.size());
                    bool occlusion_transform_applied = false;
                    bool occlusion_texcoord1_used = false;
                    bool unsupported_occlusion_texcoord = false;
                    const auto occlusion_uv = transformed_texture_uv(
                        primitive,
                        material->occlusion_texcoord,
                        material->occlusion_transform,
                        i,
                        &occlusion_transform_applied,
                        &occlusion_texcoord1_used,
                        &unsupported_occlusion_texcoord);
                    draw.vertices[i].occlusion_u = occlusion_uv.first;
                    draw.vertices[i].occlusion_v = occlusion_uv.second;
                    draw.non_base_color_texture_transform_applied =
                        draw.non_base_color_texture_transform_applied ||
                        (has_occlusion_texture &&
                         occlusion_transform_applied);
                    draw.non_base_color_texcoord1_used =
                        draw.non_base_color_texcoord1_used ||
                        (has_occlusion_texture && occlusion_texcoord1_used);
                    draw.non_base_color_texcoord1_deferred =
                        draw.non_base_color_texcoord1_deferred ||
                        unsupported_occlusion_texcoord;

                    const bool has_emissive_texture =
                        pulp::scene::is_valid_scene_index(
                            material->emissive_texture,
                            scene.textures.size());
                    bool emissive_transform_applied = false;
                    bool emissive_texcoord1_used = false;
                    bool unsupported_emissive_texcoord = false;
                    const auto emissive_uv = transformed_texture_uv(
                        primitive,
                        material->emissive_texcoord,
                        material->emissive_transform,
                        i,
                        &emissive_transform_applied,
                        &emissive_texcoord1_used,
                        &unsupported_emissive_texcoord);
                    draw.vertices[i].emissive_u = emissive_uv.first;
                    draw.vertices[i].emissive_v = emissive_uv.second;
                    draw.non_base_color_texture_transform_applied =
                        draw.non_base_color_texture_transform_applied ||
                        (has_emissive_texture &&
                         emissive_transform_applied);
                    draw.non_base_color_texcoord1_used =
                        draw.non_base_color_texcoord1_used ||
                        (has_emissive_texture && emissive_texcoord1_used);
                    draw.non_base_color_texcoord1_deferred =
                        draw.non_base_color_texcoord1_deferred ||
                        unsupported_emissive_texcoord;
                } else {
                    draw.vertices[i].metallic_roughness_u = uv.first;
                    draw.vertices[i].metallic_roughness_v = uv.second;
                    draw.vertices[i].normal_u = uv.first;
                    draw.vertices[i].normal_v = uv.second;
                    draw.vertices[i].occlusion_u = uv.first;
                    draw.vertices[i].occlusion_v = uv.second;
                    draw.vertices[i].emissive_u = uv.first;
                    draw.vertices[i].emissive_v = uv.second;
                }
                if (primitive.color0.size() >= (i + 1u) * 4u) {
                    draw.vertices[i].r = primitive.color0[i * 4u + 0u];
                    draw.vertices[i].g = primitive.color0[i * 4u + 1u];
                    draw.vertices[i].b = primitive.color0[i * 4u + 2u];
                    draw.vertices[i].a = primitive.color0[i * 4u + 3u];
                    draw.vertex_color_applied = true;
                }
                if (has_geometry_normals) {
                    const auto normal = transform_direction(
                        render_primitive.world_transform,
                        primitive.normals[i * 3u + 0u],
                        primitive.normals[i * 3u + 1u],
                        primitive.normals[i * 3u + 2u]);
                    draw.vertices[i].nx = normal.x;
                    draw.vertices[i].ny = normal.y;
                    draw.vertices[i].nz = normal.z;
                    normalize_direction(draw.vertices[i].nx,
                                        draw.vertices[i].ny,
                                        draw.vertices[i].nz);
                }
                if (primitive.tangents.size() >= (i + 1u) * 4u) {
                    const auto tangent = transform_direction(
                        render_primitive.world_transform,
                        primitive.tangents[i * 4u + 0u],
                        primitive.tangents[i * 4u + 1u],
                        primitive.tangents[i * 4u + 2u]);
                    draw.vertices[i].tx = tangent.x;
                    draw.vertices[i].ty = tangent.y;
                    draw.vertices[i].tz = tangent.z;
                    draw.vertices[i].tw = primitive.tangents[i * 4u + 3u];
                    normalize_direction(draw.vertices[i].tx,
                                        draw.vertices[i].ty,
                                        draw.vertices[i].tz);
                }

                min_x = std::min(min_x, point.x);
                min_y = std::min(min_y, point.y);
                min_z = std::min(min_z, point.z);
                max_x = std::max(max_x, point.x);
                max_y = std::max(max_y, point.y);
                max_z = std::max(max_z, point.z);
            }

            draw.indices = primitive.indices;

            if (!draw.tangent_attributes_available &&
                has_geometry_normals &&
                material != nullptr &&
                pulp::scene::is_valid_scene_index(material->normal_texture,
                                                  scene.textures.size()) &&
                texture_uv_route_supported(primitive,
                                           material->normal_texcoord,
                                           material->normal_transform) &&
                derive_tangents_from_positions_normals_uvs(draw.vertices,
                                                           draw.indices)) {
                draw.tangent_attributes_available = true;
                draw.tangent_attributes_derived = true;
            }

            if (material &&
                pulp::scene::is_valid_scene_index(material->base_color_texture,
                                                  scene.textures.size())) {
                if (auto decoded = decode_texture_rgba(
                        scene.textures[material->base_color_texture])) {
                    draw.texture = std::move(*decoded);
                }
                if (pulp::scene::is_valid_scene_index(material->base_color_sampler,
                                                      scene.texture_samplers.size())) {
                    draw.sampler = &scene.texture_samplers[material->base_color_sampler];
                }
            }
            if (draw.texture.rgba.empty()) {
                draw.texture = make_fallback_texture();
            }

            if (material &&
                pulp::scene::is_valid_scene_index(material->normal_texture,
                                                  scene.textures.size())) {
                const bool normal_uv_supported = texture_uv_route_supported(
                    primitive,
                    material->normal_texcoord,
                    material->normal_transform);
                if (auto decoded = decode_texture_rgba(
                        scene.textures[material->normal_texture])) {
                    draw.normal_texture = std::move(*decoded);
                    draw.normal_texture_applied =
                        draw.tangent_attributes_available &&
                        normal_uv_supported;
                    draw.normal_scale_applied =
                        draw.normal_texture_applied &&
                        material->normal_scale != 1.0f;
                    draw.normal_texture_deferred =
                        !draw.tangent_attributes_available ||
                        !normal_uv_supported;
                    draw.normal_scale_deferred =
                        (!draw.tangent_attributes_available ||
                         !normal_uv_supported) &&
                        material->normal_scale != 1.0f;
                }
                if (pulp::scene::is_valid_scene_index(material->normal_sampler,
                                                      scene.texture_samplers.size())) {
                    draw.normal_sampler =
                        &scene.texture_samplers[material->normal_sampler];
                }
                if (!normal_uv_supported) {
                    draw.normal_texture_deferred = true;
                    draw.normal_scale_deferred =
                        material->normal_scale != 1.0f;
                    draw.non_base_color_texcoord1_deferred = true;
                }
            }
            if (draw.normal_texture.rgba.empty()) {
                draw.normal_texture = make_solid_texture(128, 128, 255, 255);
            }

            if (material &&
                pulp::scene::is_valid_scene_index(
                    material->metallic_roughness_texture,
                    scene.textures.size())) {
                const bool metallic_roughness_uv_supported =
                    texture_uv_route_supported(
                        primitive,
                        material->metallic_roughness_texcoord,
                        material->metallic_roughness_transform);
                if (auto decoded = decode_texture_rgba(
                        scene.textures[material->metallic_roughness_texture])) {
                    draw.metallic_roughness_texture = std::move(*decoded);
                    draw.metallic_roughness_texture_applied =
                        metallic_roughness_uv_supported;
                    draw.metallic_roughness_texture_deferred =
                        !metallic_roughness_uv_supported;
                }
                if (pulp::scene::is_valid_scene_index(
                        material->metallic_roughness_sampler,
                        scene.texture_samplers.size())) {
                    draw.metallic_roughness_sampler =
                        &scene.texture_samplers[
                            material->metallic_roughness_sampler];
                }
                if (!metallic_roughness_uv_supported) {
                    draw.metallic_roughness_texture_deferred = true;
                    draw.non_base_color_texcoord1_deferred = true;
                }
            }
            if (draw.metallic_roughness_texture.rgba.empty()) {
                draw.metallic_roughness_texture =
                    make_solid_texture(255, 255, 255, 255);
            }

            if (material &&
                pulp::scene::is_valid_scene_index(material->occlusion_texture,
                                                  scene.textures.size())) {
                const bool occlusion_uv_supported = texture_uv_route_supported(
                    primitive,
                    material->occlusion_texcoord,
                    material->occlusion_transform);
                if (auto decoded = decode_texture_rgba(
                        scene.textures[material->occlusion_texture])) {
                    draw.occlusion_texture = std::move(*decoded);
                    draw.occlusion_texture_applied = occlusion_uv_supported;
                    draw.occlusion_strength_applied =
                        occlusion_uv_supported &&
                        material->occlusion_strength != 1.0f;
                    draw.occlusion_texture_deferred = !occlusion_uv_supported;
                    draw.occlusion_strength_deferred =
                        !occlusion_uv_supported &&
                        material->occlusion_strength != 1.0f;
                }
                if (pulp::scene::is_valid_scene_index(material->occlusion_sampler,
                                                      scene.texture_samplers.size())) {
                    draw.occlusion_sampler =
                        &scene.texture_samplers[material->occlusion_sampler];
                }
                if (!occlusion_uv_supported) {
                    draw.occlusion_texture_deferred = true;
                    draw.occlusion_strength_deferred =
                        material->occlusion_strength != 1.0f;
                    draw.non_base_color_texcoord1_deferred = true;
                }
            }
            if (draw.occlusion_texture.rgba.empty()) {
                draw.occlusion_texture = make_solid_texture(255, 255, 255, 255);
            }

            if (material &&
                pulp::scene::is_valid_scene_index(material->emissive_texture,
                                                  scene.textures.size())) {
                const bool emissive_uv_supported = texture_uv_route_supported(
                    primitive,
                    material->emissive_texcoord,
                    material->emissive_transform);
                if (auto decoded = decode_texture_rgba(
                        scene.textures[material->emissive_texture])) {
                    draw.emissive_texture = std::move(*decoded);
                    draw.emissive_texture_applied = emissive_uv_supported;
                    draw.emissive_texture_deferred = !emissive_uv_supported;
                }
                if (pulp::scene::is_valid_scene_index(material->emissive_sampler,
                                                      scene.texture_samplers.size())) {
                    draw.emissive_sampler =
                        &scene.texture_samplers[material->emissive_sampler];
                }
                if (!emissive_uv_supported) {
                    draw.emissive_texture_deferred = true;
                    draw.non_base_color_texcoord1_deferred = true;
                }
            }
            if (draw.emissive_texture.rgba.empty()) {
                draw.emissive_texture = make_solid_texture(0, 0, 0, 255);
            }

            draws.push_back(std::move(draw));
    }

    if (!draws.empty()) {
        const float center_x = (min_x + max_x) * 0.5f;
        const float center_y = (min_y + max_y) * 0.5f;
        const float center_z = (min_z + max_z) * 0.5f;
        const float extent = std::max({
            max_x - min_x,
            max_y - min_y,
            max_z - min_z,
            0.001f,
        });
        const float scale = 1.45f / extent;
        if (normalization != nullptr) {
            normalization->center[0] = center_x;
            normalization->center[1] = center_y;
            normalization->center[2] = center_z;
            normalization->scale = scale;
        }
        for (auto& draw : draws) {
            for (auto& vertex : draw.vertices) {
                vertex.x = (vertex.x - center_x) * scale;
                vertex.y = (vertex.y - center_y) * scale;
                vertex.z = (vertex.z - center_z) * scale;
            }
            if (!draw.vertices.empty()) {
                float depth = 0.0f;
                for (const auto& vertex : draw.vertices) {
                    depth += vertex.z;
                }
                draw.alpha_sort_depth =
                    depth / static_cast<float>(draw.vertices.size());
            }
        }
        return draws;
    }

    if (error) {
        *error = "Renderer3D: SceneData has no renderable indexed primitive";
    }
    return {};
}

#endif

} // namespace

#if defined(PULP_HAS_SKIA) && defined(PULP_HAS_WEBGPU)
namespace {

GpuSurface::AdapterBackendPreference map_backend_preference(
    Renderer3DAdapterBackendPreference preference) {
    switch (preference) {
        case Renderer3DAdapterBackendPreference::null_backend:
            return GpuSurface::AdapterBackendPreference::null_backend;
        case Renderer3DAdapterBackendPreference::default_backend:
            return GpuSurface::AdapterBackendPreference::default_backend;
    }
    return GpuSurface::AdapterBackendPreference::default_backend;
}

// Owns the Dawn frame resources every offscreen Renderer3D entry point needs:
// the GpuSurface plus its device/queue/instance handles and the color + depth
// render targets. Held by value so the device outlives the render.
struct FrameContext {
    std::unique_ptr<GpuSurface> gpu;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Instance instance;
    wgpu::Texture color_texture;
    wgpu::TextureView color_view;
    wgpu::Texture depth_texture;
    wgpu::TextureView depth_view;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Create + initialize an offscreen Dawn surface and its color/depth targets,
// recording the gpu_available / adapter-info / *_target_allocated result flags.
// `color_label` / `depth_label` name the two textures for GPU debugging.
// Returns nullopt (with result.error set) on any failure.
template <typename Config>
std::optional<FrameContext> acquire_dawn_frame(
    const Config& config, Scene3DRenderResult& result,
    const char* color_label, const char* depth_label) {
    FrameContext ctx;
    ctx.width = config.width;
    ctx.height = config.height;

    ctx.gpu = GpuSurface::create_dawn();
    if (!ctx.gpu) {
        result.error = "Renderer3D: failed to create Dawn GpuSurface";
        return std::nullopt;
    }

    GpuSurface::Config gpu_config;
    gpu_config.width = config.width;
    gpu_config.height = config.height;
    gpu_config.native_surface_handle = nullptr;
    gpu_config.force_fallback_adapter = config.force_fallback_adapter;
    gpu_config.backend_preference =
        map_backend_preference(config.backend_preference);
    if (!ctx.gpu->initialize(gpu_config)) {
        result.error = "Renderer3D: failed to initialize offscreen Dawn surface";
        return std::nullopt;
    }
    result.gpu_available = true;
    record_adapter_info(*ctx.gpu, result);

    auto* device_ptr = static_cast<wgpu::Device*>(ctx.gpu->dawn_device_handle());
    auto* queue_ptr = static_cast<wgpu::Queue*>(ctx.gpu->dawn_queue_handle());
    auto* instance_ptr = static_cast<wgpu::Instance*>(ctx.gpu->dawn_instance_handle());
    if (device_ptr == nullptr || queue_ptr == nullptr || instance_ptr == nullptr ||
        !(*device_ptr) || !(*queue_ptr) || !(*instance_ptr)) {
        result.error = "Renderer3D: missing Dawn device, queue, or instance";
        return std::nullopt;
    }
    ctx.device = *device_ptr;
    ctx.queue = *queue_ptr;
    ctx.instance = *instance_ptr;

    wgpu::TextureDescriptor color_desc{};
    color_desc.label = color_label;
    color_desc.dimension = wgpu::TextureDimension::e2D;
    color_desc.size = {config.width, config.height, 1};
    color_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    color_desc.mipLevelCount = 1;
    color_desc.sampleCount = 1;
    color_desc.usage = wgpu::TextureUsage::RenderAttachment |
                       wgpu::TextureUsage::CopySrc;
    ctx.color_texture = ctx.device.CreateTexture(&color_desc);
    if (!ctx.color_texture) {
        result.error = "Renderer3D: failed to allocate color target";
        return std::nullopt;
    }
    ctx.color_view = ctx.color_texture.CreateView();
    result.color_target_allocated = ctx.color_view != nullptr;
    if (!result.color_target_allocated) {
        result.error = "Renderer3D: failed to create color target view";
        return std::nullopt;
    }

    wgpu::TextureDescriptor depth_desc{};
    depth_desc.label = depth_label;
    depth_desc.dimension = wgpu::TextureDimension::e2D;
    depth_desc.size = {config.width, config.height, 1};
    depth_desc.format = wgpu::TextureFormat::Depth24Plus;
    depth_desc.mipLevelCount = 1;
    depth_desc.sampleCount = 1;
    depth_desc.usage = wgpu::TextureUsage::RenderAttachment;
    ctx.depth_texture = ctx.device.CreateTexture(&depth_desc);
    if (!ctx.depth_texture) {
        result.error = "Renderer3D: failed to allocate depth target";
        return std::nullopt;
    }
    ctx.depth_view = ctx.depth_texture.CreateView();
    result.depth_target_allocated = ctx.depth_view != nullptr;
    if (!result.depth_target_allocated) {
        result.error = "Renderer3D: failed to create depth target view";
        return std::nullopt;
    }

    return ctx;
}

// Blocking readback of the rendered color target into result.rgba: waits for
// the MapAsync to resolve (spinning the Dawn event loop on ctx.instance),
// de-pads each row, tallies the distinct-color / non-transparent-pixel census,
// and PNG-encodes the frame. Returns false (result.error set) on map timeout, a
// null mapping, or PNG-encode failure. Not real-time safe.
bool finish_frame_readback(FrameContext& ctx,
                           Scene3DRenderResult& result,
                           const wgpu::Buffer& readback_buffer,
                           uint64_t readback_size,
                           uint32_t padded_bytes_per_row,
                           uint32_t compact_bytes_per_row) {
    bool mapped = false;
    bool map_ok = false;
    readback_buffer.MapAsync(wgpu::MapMode::Read,
                             0,
                             readback_size,
                             wgpu::CallbackMode::AllowProcessEvents,
                             [&mapped, &map_ok](wgpu::MapAsyncStatus status,
                                                wgpu::StringView) {
                                 mapped = true;
                                 map_ok = (status == wgpu::MapAsyncStatus::Success);
                             });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!mapped && std::chrono::steady_clock::now() < deadline) {
        ctx.instance.ProcessEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!mapped || !map_ok) {
        result.error = "Renderer3D: readback map failed or timed out";
        return false;
    }

    const auto* padded = static_cast<const uint8_t*>(
        readback_buffer.GetConstMappedRange(0, readback_size));
    if (padded == nullptr) {
        result.error = "Renderer3D: mapped readback returned null";
        readback_buffer.Unmap();
        return false;
    }

    result.rgba.resize(static_cast<size_t>(compact_bytes_per_row) * ctx.height);
    for (uint32_t y = 0; y < ctx.height; ++y) {
        std::memcpy(result.rgba.data() + static_cast<size_t>(y) * compact_bytes_per_row,
                    padded + static_cast<size_t>(y) * padded_bytes_per_row,
                    compact_bytes_per_row);
    }
    readback_buffer.Unmap();
    result.readback_completed = true;

    std::unordered_set<uint32_t> colors;
    uint32_t non_transparent = 0;
    for (size_t i = 0; i + 3 < result.rgba.size(); i += 4) {
        const uint32_t color =
            static_cast<uint32_t>(result.rgba[i]) |
            (static_cast<uint32_t>(result.rgba[i + 1]) << 8u) |
            (static_cast<uint32_t>(result.rgba[i + 2]) << 16u) |
            (static_cast<uint32_t>(result.rgba[i + 3]) << 24u);
        colors.insert(color);
        if (result.rgba[i + 3] != 0) {
            ++non_transparent;
        }
    }
    result.distinct_color_count = static_cast<uint32_t>(colors.size());
    result.non_transparent_pixel_count = non_transparent;

    HeadlessSurface::Rgba rgba;
    rgba.width = ctx.width;
    rgba.height = ctx.height;
    rgba.pixels = result.rgba;
    std::string png_error;
    result.png = HeadlessSurface::encode_png(rgba, &png_error);
    if (result.png.empty()) {
        result.error = png_error.empty()
            ? "Renderer3D: PNG encode failed"
            : png_error;
        return false;
    }
    return true;
}

// Scene-wide CPU analysis every later phase reads: the renderable primitives
// plus the single directional / point / spot light and camera the scene shader
// supports. The primitives hold sampler pointers into the caller's normalized
// scene, so that scene must outlive the analysis.
struct SceneAnalysis {
    std::vector<CpuPrimitive> primitives;
    std::vector<pulp::scene::TransformedNode> transformed_nodes;
    SceneNormalization normalization;
    CpuDirectionalLight directional_light;
    CpuPointLight point_light;
    CpuSpotLight spot_light;
    CpuCameraProjection camera_projection;
};

// Roll one primitive's material / texture feature flags into the scene-wide
// result. Every flag is sticky: a feature counts as applied for the scene when
// any single primitive uses it.
void merge_primitive_feature_flags(Scene3DRenderResult& result,
                                   const CpuPrimitive& primitive) {
    result.texture_decoded =
        result.texture_decoded ||
        primitive.texture.decoded ||
        primitive.normal_texture.decoded ||
        primitive.metallic_roughness_texture.decoded ||
        primitive.occlusion_texture.decoded ||
        primitive.emissive_texture.decoded;
    result.fallback_texture_used =
        result.fallback_texture_used || primitive.texture.fallback;
    result.texture_sampler_applied =
        result.texture_sampler_applied || primitive.sampler != nullptr;
    result.base_color_transform_applied =
        result.base_color_transform_applied ||
        primitive.base_color_transform_applied;
    result.base_color_texcoord1_used =
        result.base_color_texcoord1_used ||
        primitive.base_color_texcoord1_used;
    result.base_color_factor_applied =
        result.base_color_factor_applied ||
        primitive.base_color_factor_applied;
    result.unlit_material_applied =
        result.unlit_material_applied || primitive.unlit;
    result.alpha_mask_applied =
        result.alpha_mask_applied || primitive.alpha_mask;
    result.alpha_blend_applied =
        result.alpha_blend_applied || primitive.alpha_blend;
    result.vertex_color_applied =
        result.vertex_color_applied || primitive.vertex_color_applied;
    result.geometry_normals_applied =
        result.geometry_normals_applied || primitive.geometry_normals_applied;
    result.metallic_roughness_factor_applied =
        result.metallic_roughness_factor_applied ||
        primitive.metallic_roughness_factor_applied;
    result.metallic_roughness_texture_applied =
        result.metallic_roughness_texture_applied ||
        primitive.metallic_roughness_texture_applied;
    result.double_sided_material_applied =
        result.double_sided_material_applied || primitive.double_sided;
    result.emissive_factor_applied =
        result.emissive_factor_applied ||
        primitive.emissive_factor_applied;
    result.emissive_strength_applied =
        result.emissive_strength_applied ||
        primitive.emissive_strength_applied;
    result.emissive_texture_applied =
        result.emissive_texture_applied ||
        primitive.emissive_texture_applied;
    result.tangent_attributes_available =
        result.tangent_attributes_available ||
        primitive.tangent_attributes_available;
    result.tangent_attributes_derived =
        result.tangent_attributes_derived ||
        primitive.tangent_attributes_derived;
    result.normal_texture_applied =
        result.normal_texture_applied ||
        primitive.normal_texture_applied;
    result.normal_scale_applied =
        result.normal_scale_applied ||
        primitive.normal_scale_applied;
    result.metallic_roughness_texture_deferred =
        result.metallic_roughness_texture_deferred ||
        primitive.metallic_roughness_texture_deferred;
    result.normal_texture_deferred =
        result.normal_texture_deferred ||
        primitive.normal_texture_deferred;
    result.normal_scale_deferred =
        result.normal_scale_deferred ||
        primitive.normal_scale_deferred;
    result.occlusion_texture_deferred =
        result.occlusion_texture_deferred ||
        primitive.occlusion_texture_deferred;
    result.occlusion_texture_applied =
        result.occlusion_texture_applied ||
        primitive.occlusion_texture_applied;
    result.occlusion_strength_applied =
        result.occlusion_strength_applied ||
        primitive.occlusion_strength_applied;
    result.occlusion_strength_deferred =
        result.occlusion_strength_deferred ||
        primitive.occlusion_strength_deferred;
    result.emissive_texture_deferred =
        result.emissive_texture_deferred ||
        primitive.emissive_texture_deferred;
    result.non_base_color_texture_transform_applied =
        result.non_base_color_texture_transform_applied ||
        primitive.non_base_color_texture_transform_applied;
    result.non_base_color_texcoord1_used =
        result.non_base_color_texcoord1_used ||
        primitive.non_base_color_texcoord1_used;
    result.non_base_color_texture_transform_deferred =
        result.non_base_color_texture_transform_deferred ||
        primitive.non_base_color_texture_transform_deferred;
    result.non_base_color_texcoord1_deferred =
        result.non_base_color_texcoord1_deferred ||
        primitive.non_base_color_texcoord1_deferred;
    result.advanced_material_extension_deferred =
        result.advanced_material_extension_deferred ||
        primitive.advanced_material_extension_deferred;
    if (primitive.sampler != nullptr) {
        result.texture_sampler_clamp_s =
            result.texture_sampler_clamp_s ||
            primitive.sampler->wrap_s ==
                pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
        result.texture_sampler_clamp_t =
            result.texture_sampler_clamp_t ||
            primitive.sampler->wrap_t ==
                pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
        result.texture_sampler_linear =
            result.texture_sampler_linear ||
            primitive.sampler->mag_filter ==
                pulp::scene::TextureSamplerData::Filter::linear ||
            primitive.sampler->min_filter ==
                pulp::scene::TextureSamplerData::Filter::linear ||
            primitive.sampler->min_filter ==
                pulp::scene::TextureSamplerData::Filter::linear_mipmap_nearest ||
            primitive.sampler->min_filter ==
                pulp::scene::TextureSamplerData::Filter::linear_mipmap_linear;
        result.texture_mipmap_filter_downgraded =
            result.texture_mipmap_filter_downgraded ||
            scene_filter_requires_mipmaps(primitive.sampler->min_filter);
    }
}

// Normalize the scene on the CPU and record every scene-level result flag:
// initial animation pose, lights, camera, deferred-feature reporting, and the
// per-primitive feature rollup. `scene` is the caller's untouched input (the
// deferred-animation and unsupported-feature reports describe it); `render_scene`
// is the mutable copy the pose is applied to and the primitives point into.
// Returns nullopt (result.error set) when the scene has no renderable primitive.
std::optional<SceneAnalysis> analyze_scene_data(
    const pulp::scene::SceneData& scene,
    pulp::scene::SceneData& render_scene,
    Scene3DRenderResult& result) {
    result.transform_animation_initial_pose_applied =
        apply_initial_animation_pose(render_scene);

    SceneAnalysis analysis;
    std::string scene_error;
    analysis.primitives = collect_renderable_primitives(render_scene,
                                                        &analysis.normalization,
                                                        &analysis.transformed_nodes,
                                                        &scene_error);
    if (analysis.primitives.empty()) {
        result.error = scene_error.empty()
            ? "Renderer3D: SceneData has no renderable primitive"
            : scene_error;
        return std::nullopt;
    }
    result.scene_data_consumed = true;
    result.primitive_count = static_cast<uint32_t>(analysis.primitives.size());

    analysis.directional_light = first_directional_light(render_scene,
                                                         analysis.transformed_nodes);
    result.directional_light_applied = analysis.directional_light.applied;
    result.directional_light_transform_applied =
        analysis.directional_light.transform_applied;
    result.light_node_transform_deferred =
        has_deferred_light_node_transform(render_scene);
    const auto punctual_lights = deferred_punctual_lights(render_scene);
    analysis.point_light = first_point_light(render_scene,
                                             analysis.transformed_nodes,
                                             analysis.normalization);
    analysis.spot_light = first_spot_light(render_scene,
                                           analysis.transformed_nodes,
                                           analysis.normalization);
    result.point_light_applied = analysis.point_light.applied;
    result.point_light_deferred =
        punctual_lights.point && !analysis.point_light.applied;
    result.spot_light_applied = analysis.spot_light.applied;
    result.spot_light_deferred =
        punctual_lights.spot && !analysis.spot_light.applied;
    result.punctual_light_range_applied =
        analysis.point_light.range_applied || analysis.spot_light.range_applied;
    result.punctual_light_range_deferred =
        punctual_lights.point_range_count >
            (analysis.point_light.range_applied ? 1u : 0u) ||
        punctual_lights.spot_range_count >
            (analysis.spot_light.range_applied ? 1u : 0u);
    result.spot_light_cone_deferred =
        punctual_lights.spot_cone && !analysis.spot_light.applied;

    analysis.camera_projection = first_camera_projection(render_scene,
                                                         analysis.transformed_nodes,
                                                         analysis.normalization);
    result.perspective_camera_applied =
        analysis.camera_projection.perspective_applied;
    result.orthographic_camera_applied =
        analysis.camera_projection.orthographic_applied;
    result.camera_node_translation_applied =
        analysis.camera_projection.node_translation_applied;
    result.camera_node_rotation_applied =
        analysis.camera_projection.node_rotation_applied;
    result.camera_aspect_ratio_applied =
        analysis.camera_projection.aspect_ratio_applied;
    result.camera_depth_range_applied =
        analysis.camera_projection.depth_range_applied;
    const auto camera_metadata = deferred_camera_metadata(render_scene);
    result.camera_aspect_ratio_deferred =
        camera_metadata.aspect_ratio &&
        !analysis.camera_projection.aspect_ratio_applied;
    result.camera_depth_range_deferred = has_deferred_camera_depth_range(
        render_scene, analysis.camera_projection.camera_index);
    result.camera_node_transform_deferred =
        has_deferred_camera_node_transform(render_scene);

    result.transform_animation_deferred = has_transform_animation(scene);
    const auto unsupported_features = deferred_unsupported_features(scene);
    result.skinning_deferred = unsupported_features.skinning;
    result.morph_target_deferred = unsupported_features.morph_target;
    result.gpu_instancing_deferred = unsupported_features.gpu_instancing;

    for (const auto& primitive : analysis.primitives) {
        merge_primitive_feature_flags(result, primitive);
    }
    return analysis;
}

// Per-primitive uniform block; layout mirrors the Uniforms struct in
// kSceneShaderWgsl below and the two must be edited together.
struct SceneUniforms {
    float angle;
    float aspect;
    float pad0;
    float pad1;
    float base_color_factor[4];
    float unlit;
    float alpha_mask;
    float alpha_cutoff;
    float normal_shading;
    float emissive_factor[4];
    float light_color[4];
    float light_direction[4];
    float point_light_color[4];
    float point_light_position[4];
    float point_light_params[4];
    float spot_light_color[4];
    float spot_light_position[4];
    float spot_light_direction[4];
    float spot_light_params[4];
    float camera_params[4];
    float camera_offset[4];
    float camera_right[4];
    float camera_up[4];
    float camera_depth[4];
    float camera_depth_params[4];
    float material_params[4];
    float emissive_texture_params[4];
    float occlusion_texture_params[4];
    float normal_texture_params[4];
};

constexpr const char* kSceneShaderWgsl = R"wgsl(
struct Uniforms {
    angle: f32,
    aspect: f32,
    pad0: f32,
    pad1: f32,
    base_color_factor: vec4f,
    unlit: f32,
    alpha_mask: f32,
    alpha_cutoff: f32,
    normal_shading: f32,
    emissive_factor: vec4f,
    light_color: vec4f,
    light_direction: vec4f,
    point_light_color: vec4f,
    point_light_position: vec4f,
    point_light_params: vec4f,
    spot_light_color: vec4f,
    spot_light_position: vec4f,
    spot_light_direction: vec4f,
    spot_light_params: vec4f,
    camera_params: vec4f,
    camera_offset: vec4f,
    camera_right: vec4f,
    camera_up: vec4f,
    camera_depth: vec4f,
    camera_depth_params: vec4f,
    material_params: vec4f,
    emissive_texture_params: vec4f,
    occlusion_texture_params: vec4f,
    normal_texture_params: vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var scene_texture: texture_2d<f32>;
@group(0) @binding(2) var scene_sampler: sampler;
@group(0) @binding(3) var emissive_texture: texture_2d<f32>;
@group(0) @binding(4) var emissive_sampler: sampler;
@group(0) @binding(5) var metallic_roughness_texture: texture_2d<f32>;
@group(0) @binding(6) var metallic_roughness_sampler: sampler;
@group(0) @binding(7) var occlusion_texture: texture_2d<f32>;
@group(0) @binding(8) var occlusion_sampler: sampler;
@group(0) @binding(9) var normal_texture: texture_2d<f32>;
@group(0) @binding(10) var normal_sampler: sampler;

struct VertexIn {
    @location(0) position: vec3f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
    @location(3) normal: vec3f,
    @location(4) tangent: vec4f,
    @location(5) normal_uv: vec2f,
    @location(6) metallic_roughness_uv: vec2f,
    @location(7) occlusion_uv: vec2f,
    @location(8) emissive_uv: vec2f,
};

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) shade: f32,
    @location(2) color: vec4f,
    @location(3) normal: vec3f,
    @location(4) local_position: vec3f,
    @location(5) tangent: vec4f,
    @location(6) normal_uv: vec2f,
    @location(7) metallic_roughness_uv: vec2f,
    @location(8) occlusion_uv: vec2f,
    @location(9) emissive_uv: vec2f,
};

@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    let cy = cos(uniforms.angle);
    let sy = sin(uniforms.angle);
    let cx = cos(0.35);
    let sx = sin(0.35);

    var p = input.position;
    let x0 = p.x * cy + p.z * sy;
    let z0 = -p.x * sy + p.z * cy;
    let y0 = p.y * cx - z0 * sx;
    let z1 = p.y * sx + z0 * cx;

    var out: VertexOut;
    let default_position = vec4f(x0 / uniforms.aspect,
                                 y0,
                                 0.5 - z1 * 0.30,
                                 1.0);
    let camera_relative = vec3f(x0, y0, z1) - uniforms.camera_offset.xyz;
    let camera_x = dot(camera_relative, uniforms.camera_right.xyz);
    let camera_y = dot(camera_relative, uniforms.camera_up.xyz);
    let camera_z = dot(camera_relative, uniforms.camera_depth.xyz);
    let camera_aspect = max(uniforms.camera_params.w, 0.0001);
    let view_z = max(1.2 - camera_z * 0.20, 0.2);
    let default_depth = 0.5 - camera_z * 0.30;
    let camera_near = max(uniforms.camera_depth_params.y, 0.0001);
    let camera_far = max(uniforms.camera_depth_params.z, camera_near + 0.0001);
    let depth_distance = max(view_z, camera_near);
    let ranged_depth = clamp((depth_distance - camera_near) /
                                 (camera_far - camera_near),
                             0.0,
                             1.0);
    let camera_depth_value = mix(default_depth,
                                 ranged_depth,
                                 uniforms.camera_depth_params.x);
    let perspective_position = vec4f(
        camera_x * uniforms.camera_params.y / (camera_aspect * view_z),
        camera_y * uniforms.camera_params.y / view_z,
        camera_depth_value,
        1.0);
    let orthographic_position = vec4f(
        camera_x * uniforms.camera_params.y / camera_aspect,
        camera_y * uniforms.camera_params.y,
        camera_depth_value,
        1.0);
    let camera_position = mix(perspective_position,
                              orthographic_position,
                              uniforms.camera_params.z);
    out.position = mix(default_position,
                       camera_position,
                       uniforms.camera_params.x);
    out.uv = input.uv;
    out.shade = clamp(0.82 - z1 * 0.22, 0.45, 1.0);
    out.color = input.color;
    out.normal = normalize(input.normal);
    out.local_position = input.position;
    out.tangent = input.tangent;
    out.normal_uv = input.normal_uv;
    out.metallic_roughness_uv = input.metallic_roughness_uv;
    out.occlusion_uv = input.occlusion_uv;
    out.emissive_uv = input.emissive_uv;
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4f {
    let texel = textureSample(scene_texture, scene_sampler, input.uv);
    let light_dir = normalize(uniforms.light_direction.xyz);
    let base_normal = normalize(input.normal);
    let tangent = normalize(input.tangent.xyz);
    let bitangent = normalize(cross(base_normal, tangent)) * input.tangent.w;
    let normal_texel = textureSample(normal_texture,
                                     normal_sampler,
                                     input.normal_uv);
    let tangent_normal_xy =
        (normal_texel.xy * 2.0 - vec2f(1.0)) * uniforms.normal_texture_params.y;
    let tangent_normal = normalize(vec3f(tangent_normal_xy,
                                         normal_texel.z * 2.0 - 1.0));
    let sampled_normal = normalize(tangent * tangent_normal.x +
                                   bitangent * tangent_normal.y +
                                   base_normal * tangent_normal.z);
    let shading_normal = normalize(mix(base_normal,
                                       sampled_normal,
                                       uniforms.normal_texture_params.x));
    let normal_light = clamp(dot(shading_normal, light_dir) * 0.55 + 0.45,
                             0.18,
                             1.0);
    let point_light_dir = normalize(uniforms.point_light_position.xyz -
                                    input.local_position);
    let point_distance = length(uniforms.point_light_position.xyz -
                                input.local_position);
    let point_range_light = mix(
        1.0,
        clamp(1.0 - point_distance / max(uniforms.point_light_params.y, 0.0001),
              0.0,
              1.0),
        uniforms.point_light_params.z);
    let point_normal_light = clamp(dot(shading_normal, point_light_dir) *
                                       0.55 + 0.45,
                                   0.18,
                                   1.0);
    let point_light_rgb = uniforms.point_light_color.rgb *
        point_normal_light * point_range_light * uniforms.point_light_params.x;
    let spot_to_fragment = normalize(input.local_position -
                                     uniforms.spot_light_position.xyz);
    let spot_light_dir = normalize(uniforms.spot_light_position.xyz -
                                   input.local_position);
    let spot_distance = length(uniforms.spot_light_position.xyz -
                               input.local_position);
    let spot_range_light = mix(
        1.0,
        clamp(1.0 - spot_distance / max(uniforms.spot_light_params.w, 0.0001),
              0.0,
              1.0),
        uniforms.spot_light_direction.w);
    let spot_cone = dot(spot_to_fragment,
                        normalize(uniforms.spot_light_direction.xyz));
    let spot_denominator = max(uniforms.spot_light_params.y -
                                   uniforms.spot_light_params.z,
                               0.0001);
    let spot_cone_light = clamp((spot_cone - uniforms.spot_light_params.z) /
                                    spot_denominator,
                                0.0,
                                1.0);
    let spot_normal_light = clamp(dot(shading_normal, spot_light_dir) *
                                      0.55 + 0.45,
                                  0.18,
                                  1.0);
    let spot_light_rgb = uniforms.spot_light_color.rgb *
        spot_normal_light * spot_cone_light * spot_range_light *
        uniforms.spot_light_params.x;
    let lit_shade = mix(input.shade, input.shade * normal_light, uniforms.normal_shading);
    let shade = mix(lit_shade, 1.0, uniforms.unlit);
    let light_rgb = mix(uniforms.light_color.rgb, vec3f(1.0), uniforms.unlit);
    let metallic_roughness_texel = textureSample(
        metallic_roughness_texture,
        metallic_roughness_sampler,
        input.metallic_roughness_uv);
    let metallic = clamp(
        uniforms.material_params.x *
            mix(1.0, metallic_roughness_texel.b, uniforms.material_params.z),
        0.0,
        1.0);
    let roughness = clamp(
        uniforms.material_params.y *
            mix(1.0, metallic_roughness_texel.g, uniforms.material_params.z),
        0.0,
        1.0);
    let pbr_energy = clamp(1.0 +
                               (1.0 - metallic) * 0.25 +
                               (1.0 - roughness) * 0.20,
                           1.0,
                           1.45);
    let material_energy = mix(pbr_energy, 1.0, uniforms.unlit);
    let alpha = texel.a * uniforms.base_color_factor.a * input.color.a;
    if uniforms.alpha_mask > 0.5 && alpha < uniforms.alpha_cutoff {
        discard;
    }
    let combined_light_rgb = min(light_rgb + point_light_rgb + spot_light_rgb,
                                 vec3f(4.0));
    let occlusion_texel = textureSample(occlusion_texture,
                                        occlusion_sampler,
                                        input.occlusion_uv);
    let occlusion = mix(
        1.0,
        mix(1.0, occlusion_texel.r, uniforms.occlusion_texture_params.y),
        uniforms.occlusion_texture_params.x);
    let lit = texel.rgb * uniforms.base_color_factor.rgb * input.color.rgb *
        shade * combined_light_rgb * material_energy * occlusion;
    let emissive_texel = textureSample(emissive_texture,
                                       emissive_sampler,
                                       input.emissive_uv);
    let emissive_rgb = mix(
        uniforms.emissive_factor.rgb,
        uniforms.emissive_factor.rgb * emissive_texel.rgb,
        uniforms.emissive_texture_params.x);
    let rgb = min(lit + emissive_rgb, vec3f(1.0));
    return vec4f(rgb,
                 alpha);
}
)wgsl";

// Vertex attribute table for SceneVertex; offsets track the field order in
// renderer3d_internal.hpp and the @location table in kSceneShaderWgsl.
std::array<wgpu::VertexAttribute, 9> scene_vertex_attributes() {
    std::array<wgpu::VertexAttribute, 9> attributes{};
    attributes[0].format = wgpu::VertexFormat::Float32x3;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = wgpu::VertexFormat::Float32x2;
    attributes[1].offset = sizeof(float) * 3u;
    attributes[1].shaderLocation = 1;
    attributes[2].format = wgpu::VertexFormat::Float32x4;
    attributes[2].offset = sizeof(float) * 5u;
    attributes[2].shaderLocation = 2;
    attributes[3].format = wgpu::VertexFormat::Float32x3;
    attributes[3].offset = sizeof(float) * 9u;
    attributes[3].shaderLocation = 3;
    attributes[4].format = wgpu::VertexFormat::Float32x4;
    attributes[4].offset = sizeof(float) * 12u;
    attributes[4].shaderLocation = 4;
    attributes[5].format = wgpu::VertexFormat::Float32x2;
    attributes[5].offset = sizeof(float) * 16u;
    attributes[5].shaderLocation = 5;
    attributes[6].format = wgpu::VertexFormat::Float32x2;
    attributes[6].offset = sizeof(float) * 18u;
    attributes[6].shaderLocation = 6;
    attributes[7].format = wgpu::VertexFormat::Float32x2;
    attributes[7].offset = sizeof(float) * 20u;
    attributes[7].shaderLocation = 7;
    attributes[8].format = wgpu::VertexFormat::Float32x2;
    attributes[8].offset = sizeof(float) * 22u;
    attributes[8].shaderLocation = 8;
    return attributes;
}

SceneUniforms make_scene_uniforms(const CpuPrimitive& primitive,
                                  const SceneAnalysis& analysis,
                                  const SceneDataRenderConfig& config) {
    return SceneUniforms{
        0.28f,
        static_cast<float>(config.width) / static_cast<float>(config.height),
        0.0f,
        0.0f,
        {
            primitive.base_color_factor[0],
            primitive.base_color_factor[1],
            primitive.base_color_factor[2],
            primitive.base_color_factor[3],
        },
        primitive.unlit ? 1.0f : 0.0f,
        primitive.alpha_mask ? 1.0f : 0.0f,
        primitive.alpha_cutoff,
        primitive.geometry_normals_applied ? 1.0f : 0.0f,
        {
            primitive.emissive_factor[0],
            primitive.emissive_factor[1],
            primitive.emissive_factor[2],
            0.0f,
        },
        {
            analysis.directional_light.color[0],
            analysis.directional_light.color[1],
            analysis.directional_light.color[2],
            1.0f,
        },
        {
            analysis.directional_light.direction[0],
            analysis.directional_light.direction[1],
            analysis.directional_light.direction[2],
            0.0f,
        },
        {
            analysis.point_light.color[0],
            analysis.point_light.color[1],
            analysis.point_light.color[2],
            1.0f,
        },
        {
            analysis.point_light.position[0],
            analysis.point_light.position[1],
            analysis.point_light.position[2],
            1.0f,
        },
        {
            analysis.point_light.applied ? 1.0f : 0.0f,
            analysis.point_light.range,
            analysis.point_light.range_applied ? 1.0f : 0.0f,
            0.0f,
        },
        {
            analysis.spot_light.color[0],
            analysis.spot_light.color[1],
            analysis.spot_light.color[2],
            1.0f,
        },
        {
            analysis.spot_light.position[0],
            analysis.spot_light.position[1],
            analysis.spot_light.position[2],
            1.0f,
        },
        {
            analysis.spot_light.direction[0],
            analysis.spot_light.direction[1],
            analysis.spot_light.direction[2],
            analysis.spot_light.range_applied ? 1.0f : 0.0f,
        },
        {
            analysis.spot_light.applied ? 1.0f : 0.0f,
            analysis.spot_light.inner_cos,
            analysis.spot_light.outer_cos,
            analysis.spot_light.range,
        },
        {
            analysis.camera_projection.applied() ? 1.0f : 0.0f,
            analysis.camera_projection.projection_scale,
            analysis.camera_projection.orthographic_applied ? 1.0f : 0.0f,
            analysis.camera_projection.aspect_ratio_applied
                ? analysis.camera_projection.aspect_ratio
                : static_cast<float>(config.width) /
                    static_cast<float>(config.height),
        },
        {
            analysis.camera_projection.camera_offset[0],
            analysis.camera_projection.camera_offset[1],
            analysis.camera_projection.camera_offset[2],
            0.0f,
        },
        {
            analysis.camera_projection.camera_right[0],
            analysis.camera_projection.camera_right[1],
            analysis.camera_projection.camera_right[2],
            0.0f,
        },
        {
            analysis.camera_projection.camera_up[0],
            analysis.camera_projection.camera_up[1],
            analysis.camera_projection.camera_up[2],
            0.0f,
        },
        {
            analysis.camera_projection.camera_depth[0],
            analysis.camera_projection.camera_depth[1],
            analysis.camera_projection.camera_depth[2],
            0.0f,
        },
        {
            analysis.camera_projection.depth_range_applied ? 1.0f : 0.0f,
            analysis.camera_projection.znear,
            analysis.camera_projection.zfar,
            0.0f,
        },
        {
            primitive.metallic_factor,
            primitive.roughness_factor,
            primitive.metallic_roughness_texture_applied ? 1.0f : 0.0f,
            0.0f,
        },
        {
            primitive.emissive_texture_applied ? 1.0f : 0.0f,
            0.0f,
            0.0f,
            0.0f,
        },
        {
            primitive.occlusion_texture_applied ? 1.0f : 0.0f,
            primitive.occlusion_strength,
            0.0f,
            0.0f,
        },
        {
            primitive.normal_texture_applied ? 1.0f : 0.0f,
            primitive.normal_scale,
            0.0f,
            0.0f,
        },
    };
}

// A GPU texture plus its default view, as bound by the scene bind group.
struct GpuSceneTexture {
    wgpu::Texture texture;
    wgpu::TextureView view;
};

// One primitive's GPU-side resources, ready to encode.
struct GpuScenePrimitive {
    wgpu::Buffer vertex_buffer;
    wgpu::Buffer index_buffer;
    wgpu::Buffer uniform_buffer;
    GpuSceneTexture base_color;
    GpuSceneTexture normal;
    GpuSceneTexture metallic_roughness;
    GpuSceneTexture occlusion;
    GpuSceneTexture emissive;
    wgpu::Sampler sampler;
    wgpu::Sampler normal_sampler;
    wgpu::Sampler metallic_roughness_sampler;
    wgpu::Sampler occlusion_sampler;
    wgpu::Sampler emissive_sampler;
    wgpu::RenderPipeline pipeline;
    wgpu::BindGroup bind_group;
    uint32_t index_count = 0;
    bool alpha_blend = false;
    float alpha_sort_depth = 0.0f;
};

// Upload one CPU texture and create its view. `label` names the texture for GPU
// debugging; `kind` names it in the failure messages and is empty for the base
// color texture. `on_written` runs once the texels have been queued, before the
// view is created. Returns nullopt (result.error set) on allocation failure.
template <typename OnWritten>
std::optional<GpuSceneTexture> upload_scene_texture(FrameContext& ctx,
                                                    const CpuTexture& source,
                                                    wgpu::TextureFormat format,
                                                    const char* label,
                                                    const char* kind,
                                                    Scene3DRenderResult& result,
                                                    OnWritten&& on_written) {
    wgpu::TextureDescriptor texture_desc{};
    texture_desc.label = label;
    texture_desc.dimension = wgpu::TextureDimension::e2D;
    texture_desc.size = {source.width, source.height, 1};
    texture_desc.format = format;
    texture_desc.mipLevelCount = 1;
    texture_desc.sampleCount = 1;
    texture_desc.usage = wgpu::TextureUsage::TextureBinding |
                         wgpu::TextureUsage::CopyDst;

    GpuSceneTexture uploaded;
    uploaded.texture = ctx.device.CreateTexture(&texture_desc);
    if (!uploaded.texture) {
        result.error =
            std::string("Renderer3D: failed to allocate ") + kind + "texture";
        return std::nullopt;
    }

    wgpu::TexelCopyTextureInfo texture_dst{};
    texture_dst.texture = uploaded.texture;
    texture_dst.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferLayout texture_layout{};
    texture_layout.bytesPerRow = source.width * 4u;
    texture_layout.rowsPerImage = source.height;
    wgpu::Extent3D texture_size{source.width, source.height, 1};
    ctx.queue.WriteTexture(&texture_dst,
                           source.rgba.data(),
                           source.rgba.size(),
                           &texture_layout,
                           &texture_size);
    on_written();

    uploaded.view = uploaded.texture.CreateView();
    if (!uploaded.view) {
        result.error =
            std::string("Renderer3D: failed to create ") + kind + "texture view";
        return std::nullopt;
    }
    return uploaded;
}

// Overlay one scene sampler's wrap / filter settings onto a Dawn sampler
// descriptor. A null sampler leaves the descriptor untouched, so a descriptor
// copied from the base color sampler keeps that sampler's settings. Filters
// that ask for mipmaps fall back to nearest: scene textures are single-level.
void apply_scene_sampler(wgpu::SamplerDescriptor& desc,
                         const pulp::scene::TextureSamplerData* sampler) {
    if (sampler == nullptr) {
        return;
    }
    desc.addressModeU = address_mode_from_scene_wrap(sampler->wrap_s);
    desc.addressModeV = address_mode_from_scene_wrap(sampler->wrap_t);
    desc.magFilter = filter_mode_from_scene_filter(sampler->mag_filter,
                                                   wgpu::FilterMode::Nearest);
    desc.minFilter = filter_mode_from_scene_filter(sampler->min_filter,
                                                   desc.magFilter);
    desc.mipmapFilter = scene_filter_requires_mipmaps(sampler->min_filter)
        ? wgpu::MipmapFilterMode::Nearest
        : mipmap_filter_mode_from_scene_filter(sampler->min_filter);
}

// `kind` names the sampler in the failure message and is empty for the base
// color sampler. Returns a null sampler (result.error set) on failure.
wgpu::Sampler create_scene_sampler(FrameContext& ctx,
                                   const wgpu::SamplerDescriptor& desc,
                                   const char* kind,
                                   Scene3DRenderResult& result) {
    auto sampler = ctx.device.CreateSampler(&desc);
    if (!sampler) {
        result.error =
            std::string("Renderer3D: failed to create ") + kind + "sampler";
    }
    return sampler;
}

// Build one GpuScenePrimitive per analyzed primitive: buffers, textures,
// samplers, a pipeline from a cache keyed on the blend / cull state, and the
// bind group. Draw order is settled here as well — opaque first, then the
// blended primitives back to front. Returns false (result.error set) on any GPU
// allocation failure, leaving the flags recorded so far in place.
bool build_scene_gpu_primitives(FrameContext& ctx,
                                const SceneDataRenderConfig& config,
                                const SceneAnalysis& analysis,
                                const wgpu::ShaderModule& shader,
                                const wgpu::VertexBufferLayout& vertex_layout,
                                const wgpu::DepthStencilState& depth_stencil,
                                std::vector<GpuScenePrimitive>& gpu_primitives,
                                Scene3DRenderResult& result) {
    gpu_primitives.reserve(analysis.primitives.size());
    std::unordered_map<ScenePipelineKey,
                       wgpu::RenderPipeline,
                       ScenePipelineKeyHash> pipeline_cache;

    for (const auto& primitive : analysis.primitives) {
        const SceneUniforms uniforms =
            make_scene_uniforms(primitive, analysis, config);

        GpuScenePrimitive gpu_primitive;
        gpu_primitive.index_count =
            static_cast<uint32_t>(primitive.indices.size());
        gpu_primitive.alpha_blend = primitive.alpha_blend;
        gpu_primitive.alpha_sort_depth = primitive.alpha_sort_depth;

        wgpu::BufferDescriptor vertex_desc{};
        vertex_desc.label = "Pulp Renderer3D SceneData vertices";
        vertex_desc.size = primitive.vertices.size() * sizeof(SceneVertex);
        vertex_desc.usage =
            wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        gpu_primitive.vertex_buffer = ctx.device.CreateBuffer(&vertex_desc);
        if (!gpu_primitive.vertex_buffer) {
            result.error = "Renderer3D: failed to allocate vertex buffer";
            return false;
        }
        ctx.queue.WriteBuffer(gpu_primitive.vertex_buffer,
                              0,
                              primitive.vertices.data(),
                              vertex_desc.size);
        result.vertex_buffer_uploaded = true;

        wgpu::BufferDescriptor index_desc{};
        index_desc.label = "Pulp Renderer3D SceneData indices";
        index_desc.size = primitive.indices.size() * sizeof(uint32_t);
        index_desc.usage =
            wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        gpu_primitive.index_buffer = ctx.device.CreateBuffer(&index_desc);
        if (!gpu_primitive.index_buffer) {
            result.error = "Renderer3D: failed to allocate index buffer";
            return false;
        }
        ctx.queue.WriteBuffer(gpu_primitive.index_buffer,
                              0,
                              primitive.indices.data(),
                              index_desc.size);
        result.index_buffer_uploaded = true;

        wgpu::BufferDescriptor uniform_desc{};
        uniform_desc.label = "Pulp Renderer3D SceneData uniforms";
        uniform_desc.size = sizeof(SceneUniforms);
        uniform_desc.usage =
            wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        gpu_primitive.uniform_buffer = ctx.device.CreateBuffer(&uniform_desc);
        if (!gpu_primitive.uniform_buffer) {
            result.error = "Renderer3D: failed to allocate uniform buffer";
            return false;
        }
        ctx.queue.WriteBuffer(gpu_primitive.uniform_buffer,
                              0,
                              &uniforms,
                              sizeof(uniforms));
        result.uniform_buffer_uploaded = true;

        auto base_color = upload_scene_texture(
            ctx, primitive.texture, wgpu::TextureFormat::RGBA8UnormSrgb,
            "Pulp Renderer3D SceneData texture", "", result,
            [&result] {
                result.texture_uploaded = true;
                result.base_color_texture_srgb_applied = true;
            });
        if (!base_color) {
            return false;
        }
        gpu_primitive.base_color = std::move(*base_color);

        auto normal = upload_scene_texture(
            ctx, primitive.normal_texture, wgpu::TextureFormat::RGBA8Unorm,
            "Pulp Renderer3D SceneData normal texture", "normal ", result,
            [] {});
        if (!normal) {
            return false;
        }
        gpu_primitive.normal = std::move(*normal);

        auto metallic_roughness = upload_scene_texture(
            ctx, primitive.metallic_roughness_texture,
            wgpu::TextureFormat::RGBA8Unorm,
            "Pulp Renderer3D SceneData metallic-roughness texture",
            "metallic-roughness ", result, [] {});
        if (!metallic_roughness) {
            return false;
        }
        gpu_primitive.metallic_roughness = std::move(*metallic_roughness);

        auto occlusion = upload_scene_texture(
            ctx, primitive.occlusion_texture, wgpu::TextureFormat::RGBA8Unorm,
            "Pulp Renderer3D SceneData occlusion texture", "occlusion ", result,
            [] {});
        if (!occlusion) {
            return false;
        }
        gpu_primitive.occlusion = std::move(*occlusion);

        auto emissive = upload_scene_texture(
            ctx, primitive.emissive_texture,
            wgpu::TextureFormat::RGBA8UnormSrgb,
            "Pulp Renderer3D SceneData emissive texture", "emissive ", result,
            [] {});
        if (!emissive) {
            return false;
        }
        gpu_primitive.emissive = std::move(*emissive);

        wgpu::SamplerDescriptor sampler_desc{};
        sampler_desc.label = "Pulp Renderer3D SceneData sampler";
        sampler_desc.addressModeU = wgpu::AddressMode::Repeat;
        sampler_desc.addressModeV = wgpu::AddressMode::Repeat;
        sampler_desc.magFilter = wgpu::FilterMode::Nearest;
        sampler_desc.minFilter = wgpu::FilterMode::Nearest;
        sampler_desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        apply_scene_sampler(sampler_desc, primitive.sampler);
        gpu_primitive.sampler =
            create_scene_sampler(ctx, sampler_desc, "", result);
        if (!gpu_primitive.sampler) {
            return false;
        }

        // Each material sampler starts from the base color descriptor, so a
        // primitive that names only a base color sampler shares its settings.
        wgpu::SamplerDescriptor normal_sampler_desc = sampler_desc;
        normal_sampler_desc.label = "Pulp Renderer3D SceneData normal sampler";
        apply_scene_sampler(normal_sampler_desc, primitive.normal_sampler);
        gpu_primitive.normal_sampler =
            create_scene_sampler(ctx, normal_sampler_desc, "normal ", result);
        if (!gpu_primitive.normal_sampler) {
            return false;
        }

        wgpu::SamplerDescriptor metallic_roughness_sampler_desc = sampler_desc;
        metallic_roughness_sampler_desc.label =
            "Pulp Renderer3D SceneData metallic-roughness sampler";
        apply_scene_sampler(metallic_roughness_sampler_desc,
                            primitive.metallic_roughness_sampler);
        gpu_primitive.metallic_roughness_sampler =
            create_scene_sampler(ctx,
                                 metallic_roughness_sampler_desc,
                                 "metallic-roughness ",
                                 result);
        if (!gpu_primitive.metallic_roughness_sampler) {
            return false;
        }

        wgpu::SamplerDescriptor occlusion_sampler_desc = sampler_desc;
        occlusion_sampler_desc.label =
            "Pulp Renderer3D SceneData occlusion sampler";
        apply_scene_sampler(occlusion_sampler_desc, primitive.occlusion_sampler);
        gpu_primitive.occlusion_sampler =
            create_scene_sampler(ctx,
                                 occlusion_sampler_desc,
                                 "occlusion ",
                                 result);
        if (!gpu_primitive.occlusion_sampler) {
            return false;
        }

        wgpu::SamplerDescriptor emissive_sampler_desc = sampler_desc;
        emissive_sampler_desc.label = "Pulp Renderer3D SceneData emissive sampler";
        apply_scene_sampler(emissive_sampler_desc, primitive.emissive_sampler);
        gpu_primitive.emissive_sampler =
            create_scene_sampler(ctx, emissive_sampler_desc, "emissive ", result);
        if (!gpu_primitive.emissive_sampler) {
            return false;
        }

        const ScenePipelineKey pipeline_key{
            primitive.alpha_blend,
            primitive.double_sided,
        };
        auto pipeline_it = pipeline_cache.find(pipeline_key);
        if (pipeline_it != pipeline_cache.end()) {
            gpu_primitive.pipeline = pipeline_it->second;
            ++result.pipeline_cache_hit_count;
        } else {
            wgpu::ColorTargetState primitive_color_target{};
            primitive_color_target.format = wgpu::TextureFormat::RGBA8Unorm;
            primitive_color_target.writeMask = wgpu::ColorWriteMask::All;
            wgpu::BlendState alpha_blend{};
            if (primitive.alpha_blend) {
                alpha_blend.color.operation = wgpu::BlendOperation::Add;
                alpha_blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
                alpha_blend.color.dstFactor =
                    wgpu::BlendFactor::OneMinusSrcAlpha;
                alpha_blend.alpha.operation = wgpu::BlendOperation::Add;
                alpha_blend.alpha.srcFactor = wgpu::BlendFactor::One;
                alpha_blend.alpha.dstFactor =
                    wgpu::BlendFactor::OneMinusSrcAlpha;
                primitive_color_target.blend = &alpha_blend;
            }

            wgpu::FragmentState primitive_fragment{};
            primitive_fragment.module = shader;
            primitive_fragment.entryPoint = "fs_main";
            primitive_fragment.targetCount = 1;
            primitive_fragment.targets = &primitive_color_target;

            wgpu::RenderPipelineDescriptor pipeline_desc{};
            wgpu::DepthStencilState primitive_depth_stencil = depth_stencil;
            if (primitive.alpha_blend) {
                primitive_depth_stencil.depthWriteEnabled = false;
                result.alpha_blend_depth_write_disabled = true;
            }
            pipeline_desc.label = "Pulp Renderer3D SceneData pipeline";
            pipeline_desc.layout = nullptr;
            pipeline_desc.vertex.module = shader;
            pipeline_desc.vertex.entryPoint = "vs_main";
            pipeline_desc.vertex.bufferCount = 1;
            pipeline_desc.vertex.buffers = &vertex_layout;
            pipeline_desc.fragment = &primitive_fragment;
            pipeline_desc.primitive.topology =
                wgpu::PrimitiveTopology::TriangleList;
            pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;
            pipeline_desc.primitive.cullMode = primitive.double_sided
                ? wgpu::CullMode::None
                : wgpu::CullMode::Back;
            pipeline_desc.depthStencil = &primitive_depth_stencil;
            pipeline_desc.multisample.count = 1;
            pipeline_desc.multisample.mask = ~0u;
            gpu_primitive.pipeline =
                ctx.device.CreateRenderPipeline(&pipeline_desc);
            if (!gpu_primitive.pipeline) {
                result.error = "Renderer3D: failed to create render pipeline";
                return false;
            }
            pipeline_cache.emplace(pipeline_key, gpu_primitive.pipeline);
        }

        std::array<wgpu::BindGroupEntry, 11> bind_entries{};
        bind_entries[0].binding = 0;
        bind_entries[0].buffer = gpu_primitive.uniform_buffer;
        bind_entries[0].offset = 0;
        bind_entries[0].size = sizeof(SceneUniforms);
        bind_entries[1].binding = 1;
        bind_entries[1].textureView = gpu_primitive.base_color.view;
        bind_entries[2].binding = 2;
        bind_entries[2].sampler = gpu_primitive.sampler;
        bind_entries[3].binding = 3;
        bind_entries[3].textureView = gpu_primitive.emissive.view;
        bind_entries[4].binding = 4;
        bind_entries[4].sampler = gpu_primitive.emissive_sampler;
        bind_entries[5].binding = 5;
        bind_entries[5].textureView = gpu_primitive.metallic_roughness.view;
        bind_entries[6].binding = 6;
        bind_entries[6].sampler = gpu_primitive.metallic_roughness_sampler;
        bind_entries[7].binding = 7;
        bind_entries[7].textureView = gpu_primitive.occlusion.view;
        bind_entries[8].binding = 8;
        bind_entries[8].sampler = gpu_primitive.occlusion_sampler;
        bind_entries[9].binding = 9;
        bind_entries[9].textureView = gpu_primitive.normal.view;
        bind_entries[10].binding = 10;
        bind_entries[10].sampler = gpu_primitive.normal_sampler;

        wgpu::BindGroupDescriptor bind_desc{};
        bind_desc.label = "Pulp Renderer3D SceneData bind group";
        bind_desc.layout = gpu_primitive.pipeline.GetBindGroupLayout(0);
        bind_desc.entryCount = bind_entries.size();
        bind_desc.entries = bind_entries.data();
        gpu_primitive.bind_group = ctx.device.CreateBindGroup(&bind_desc);
        if (!gpu_primitive.bind_group) {
            result.error = "Renderer3D: failed to create bind group";
            return false;
        }

        gpu_primitives.push_back(std::move(gpu_primitive));
    }

    result.pipeline_cache_entry_count =
        static_cast<uint32_t>(pipeline_cache.size());
    const auto blended_count = static_cast<size_t>(std::count_if(
        gpu_primitives.begin(),
        gpu_primitives.end(),
        [](const GpuScenePrimitive& primitive) {
            return primitive.alpha_blend;
        }));
    std::stable_sort(gpu_primitives.begin(),
                     gpu_primitives.end(),
                     [](const GpuScenePrimitive& lhs,
                        const GpuScenePrimitive& rhs) {
                         if (lhs.alpha_blend != rhs.alpha_blend) {
                             return !lhs.alpha_blend && rhs.alpha_blend;
                         }
                         if (lhs.alpha_blend && rhs.alpha_blend) {
                             return lhs.alpha_sort_depth < rhs.alpha_sort_depth;
                         }
                         return false;
                     });
    result.alpha_blend_sorted = blended_count > 1u;
    return true;
}

// The mappable buffer the encoded frame lands in, plus the row stride
// finish_frame_readback needs to de-pad it.
struct SceneReadback {
    wgpu::Buffer buffer;
    uint64_t size = 0;
    uint32_t padded_bytes_per_row = 0;
    uint32_t compact_bytes_per_row = 0;
};

// Encode and submit the scene pass: clear, draw every primitive in the order
// build_scene_gpu_primitives settled, then copy the color target into a
// mappable readback buffer. Returns nullopt (result.error set) when a GPU
// object fails to allocate.
std::optional<SceneReadback> encode_scene_pass(
    FrameContext& ctx,
    const SceneDataRenderConfig& config,
    const std::vector<GpuScenePrimitive>& gpu_primitives,
    Scene3DRenderResult& result) {
    wgpu::RenderPassColorAttachment color_attachment{};
    color_attachment.view = ctx.color_view;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    color_attachment.clearValue = {0.02, 0.025, 0.035, 1.0};

    wgpu::RenderPassDepthStencilAttachment depth_attachment{};
    depth_attachment.view = ctx.depth_view;
    depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.depthClearValue = 1.0f;

    wgpu::RenderPassDescriptor pass_desc{};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_attachment;
    pass_desc.depthStencilAttachment = &depth_attachment;

    const uint32_t bytes_per_pixel = 4;
    SceneReadback readback;
    readback.compact_bytes_per_row = config.width * bytes_per_pixel;
    readback.padded_bytes_per_row =
        align_to(readback.compact_bytes_per_row, 256);
    readback.size =
        static_cast<uint64_t>(readback.padded_bytes_per_row) * config.height;
    wgpu::BufferDescriptor readback_desc{};
    readback_desc.label = "Pulp Renderer3D SceneData readback";
    readback_desc.size = readback.size;
    readback_desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    readback.buffer = ctx.device.CreateBuffer(&readback_desc);
    if (!readback.buffer) {
        result.error = "Renderer3D: failed to allocate readback buffer";
        return std::nullopt;
    }

    wgpu::CommandEncoderDescriptor encoder_desc{};
    auto encoder = ctx.device.CreateCommandEncoder(&encoder_desc);
    if (!encoder) {
        result.error = "Renderer3D: failed to create command encoder";
        return std::nullopt;
    }
    auto pass = encoder.BeginRenderPass(&pass_desc);
    for (const auto& gpu_primitive : gpu_primitives) {
        pass.SetPipeline(gpu_primitive.pipeline);
        pass.SetBindGroup(0, gpu_primitive.bind_group);
        pass.SetVertexBuffer(0, gpu_primitive.vertex_buffer);
        pass.SetIndexBuffer(gpu_primitive.index_buffer,
                            wgpu::IndexFormat::Uint32);
        pass.DrawIndexed(gpu_primitive.index_count, 1, 0, 0, 0);
    }
    pass.End();

    wgpu::TexelCopyTextureInfo copy_src{};
    copy_src.texture = ctx.color_texture;
    copy_src.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferInfo copy_dst{};
    copy_dst.buffer = readback.buffer;
    copy_dst.layout.bytesPerRow = readback.padded_bytes_per_row;
    copy_dst.layout.rowsPerImage = config.height;
    wgpu::Extent3D copy_size{config.width, config.height, 1};
    encoder.CopyTextureToBuffer(&copy_src, &copy_dst, &copy_size);

    auto command_buffer = encoder.Finish();
    ctx.queue.Submit(1, &command_buffer);
    result.command_submitted = true;
    return readback;
}

// A scene render only counts as successful when every stage landed and the
// frame is not blank. When it fell short without a specific error, name the
// stages that are missing.
void finalize_scene_result(Scene3DRenderResult& result) {
    result.success = result.scene_data_consumed &&
                     result.color_target_allocated &&
                     result.depth_target_allocated &&
                     result.vertex_buffer_uploaded &&
                     result.index_buffer_uploaded &&
                     result.uniform_buffer_uploaded &&
                     result.texture_uploaded &&
                     result.command_submitted &&
                     result.readback_completed &&
                     result.distinct_color_count > 1 &&
                     result.non_transparent_pixel_count > 0;
    if (!result.success && result.error.empty()) {
        result.error = "Renderer3D: SceneData render incomplete:";
        auto append_missing = [&](bool condition, const char* name) {
            if (!condition) {
                result.error += " ";
                result.error += name;
            }
        };
        append_missing(result.scene_data_consumed, "scene_data_consumed");
        append_missing(result.color_target_allocated, "color_target_allocated");
        append_missing(result.depth_target_allocated, "depth_target_allocated");
        append_missing(result.vertex_buffer_uploaded, "vertex_buffer_uploaded");
        append_missing(result.index_buffer_uploaded, "index_buffer_uploaded");
        append_missing(result.uniform_buffer_uploaded, "uniform_buffer_uploaded");
        append_missing(result.texture_uploaded, "texture_uploaded");
        append_missing(result.command_submitted, "command_submitted");
        append_missing(result.readback_completed, "readback_completed");
        append_missing(result.distinct_color_count > 1, "distinct_color_count");
        append_missing(result.non_transparent_pixel_count > 0,
                       "non_transparent_pixel_count");
    }
}

} // namespace
#endif

Scene3DRenderResult Renderer3D::render_hardcoded_textured_cube(
    const HardcodedCubeRenderConfig& config) {
    Scene3DRenderResult result;
    result.width = config.width;
    result.height = config.height;
    result.fallback_adapter_requested = config.force_fallback_adapter;
    result.null_backend_requested =
        config.backend_preference ==
        Renderer3DAdapterBackendPreference::null_backend;

    if (config.width == 0 || config.height == 0) {
        result.error = "Renderer3D: render size must be non-zero";
        return result;
    }

#if !defined(PULP_HAS_SKIA) || !defined(PULP_HAS_WEBGPU)
    result.error = "Renderer3D: built without Dawn/WebGPU";
    return result;
#else
    auto frame = acquire_dawn_frame(config, result,
                                    "Pulp Renderer3D hardcoded cube color",
                                    "Pulp Renderer3D hardcoded cube depth");
    if (!frame) return result;
    auto& ctx = *frame;
    auto& device = ctx.device;
    auto& queue = ctx.queue;
    auto& color_texture = ctx.color_texture;
    auto& color_view = ctx.color_view;
    auto& depth_view = ctx.depth_view;

    struct Vertex {
        float x, y, z;
        float u, v;
    };

    const std::array<Vertex, 24> vertices = {{
        {-0.5f, -0.5f,  0.5f, 0.0f, 1.0f}, { 0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f, 1.0f, 0.0f}, {-0.5f,  0.5f,  0.5f, 0.0f, 0.0f},
        { 0.5f, -0.5f, -0.5f, 0.0f, 1.0f}, {-0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        {-0.5f,  0.5f, -0.5f, 1.0f, 0.0f}, { 0.5f,  0.5f, -0.5f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, 1.0f}, {-0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        {-0.5f,  0.5f,  0.5f, 1.0f, 0.0f}, {-0.5f,  0.5f, -0.5f, 0.0f, 0.0f},
        { 0.5f, -0.5f,  0.5f, 0.0f, 1.0f}, { 0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f, 1.0f, 0.0f}, { 0.5f,  0.5f,  0.5f, 0.0f, 0.0f},
        {-0.5f,  0.5f,  0.5f, 0.0f, 1.0f}, { 0.5f,  0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f, 1.0f, 0.0f}, {-0.5f,  0.5f, -0.5f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, 1.0f}, { 0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f, 1.0f, 0.0f}, {-0.5f, -0.5f,  0.5f, 0.0f, 0.0f},
    }};

    const std::array<uint16_t, 36> indices = {{
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23,
    }};

    struct Uniforms {
        float angle;
        float aspect;
        float pad0;
        float pad1;
    };
    const Uniforms uniforms{0.65f,
                            static_cast<float>(config.width) /
                                static_cast<float>(config.height),
                            0.0f,
                            0.0f};

    wgpu::BufferDescriptor vertex_desc{};
    vertex_desc.label = "Pulp Renderer3D cube vertices";
    vertex_desc.size = sizeof(vertices);
    vertex_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    auto vertex_buffer = device.CreateBuffer(&vertex_desc);
    if (!vertex_buffer) {
        result.error = "Renderer3D: failed to allocate vertex buffer";
        return result;
    }
    queue.WriteBuffer(vertex_buffer, 0, vertices.data(), sizeof(vertices));
    result.vertex_buffer_uploaded = true;

    wgpu::BufferDescriptor index_desc{};
    index_desc.label = "Pulp Renderer3D cube indices";
    index_desc.size = sizeof(indices);
    index_desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    auto index_buffer = device.CreateBuffer(&index_desc);
    if (!index_buffer) {
        result.error = "Renderer3D: failed to allocate index buffer";
        return result;
    }
    queue.WriteBuffer(index_buffer, 0, indices.data(), sizeof(indices));
    result.index_buffer_uploaded = true;

    wgpu::BufferDescriptor uniform_desc{};
    uniform_desc.label = "Pulp Renderer3D cube uniforms";
    uniform_desc.size = sizeof(Uniforms);
    uniform_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    auto uniform_buffer = device.CreateBuffer(&uniform_desc);
    if (!uniform_buffer) {
        result.error = "Renderer3D: failed to allocate uniform buffer";
        return result;
    }
    queue.WriteBuffer(uniform_buffer, 0, &uniforms, sizeof(uniforms));
    result.uniform_buffer_uploaded = true;

    const std::array<uint8_t, 4 * 4 * 4> texture_bytes = {{
        255,  40,  40, 255,   40, 255,  80, 255,  255,  40,  40, 255,   40, 255,  80, 255,
         50, 100, 255, 255,  255, 235,  40, 255,   50, 100, 255, 255,  255, 235,  40, 255,
        255,  40,  40, 255,   40, 255,  80, 255,  255,  40,  40, 255,   40, 255,  80, 255,
         50, 100, 255, 255,  255, 235,  40, 255,   50, 100, 255, 255,  255, 235,  40, 255,
    }};

    wgpu::TextureDescriptor texture_desc{};
    texture_desc.label = "Pulp Renderer3D cube texture";
    texture_desc.dimension = wgpu::TextureDimension::e2D;
    texture_desc.size = {4, 4, 1};
    texture_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    texture_desc.mipLevelCount = 1;
    texture_desc.sampleCount = 1;
    texture_desc.usage = wgpu::TextureUsage::TextureBinding |
                         wgpu::TextureUsage::CopyDst;
    auto texture = device.CreateTexture(&texture_desc);
    if (!texture) {
        result.error = "Renderer3D: failed to allocate texture";
        return result;
    }
    wgpu::TexelCopyTextureInfo texture_dst{};
    texture_dst.texture = texture;
    texture_dst.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferLayout texture_layout{};
    texture_layout.bytesPerRow = 4 * 4;
    texture_layout.rowsPerImage = 4;
    wgpu::Extent3D texture_size{4, 4, 1};
    queue.WriteTexture(&texture_dst,
                       texture_bytes.data(),
                       texture_bytes.size(),
                       &texture_layout,
                       &texture_size);
    result.texture_uploaded = true;
    auto texture_view = texture.CreateView();
    if (!texture_view) {
        result.error = "Renderer3D: failed to create texture view";
        return result;
    }

    wgpu::SamplerDescriptor sampler_desc{};
    sampler_desc.label = "Pulp Renderer3D cube sampler";
    sampler_desc.addressModeU = wgpu::AddressMode::Repeat;
    sampler_desc.addressModeV = wgpu::AddressMode::Repeat;
    sampler_desc.magFilter = wgpu::FilterMode::Nearest;
    sampler_desc.minFilter = wgpu::FilterMode::Nearest;
    auto sampler = device.CreateSampler(&sampler_desc);
    if (!sampler) {
        result.error = "Renderer3D: failed to create sampler";
        return result;
    }

    static constexpr const char* kShader = R"wgsl(
struct Uniforms {
    angle: f32,
    aspect: f32,
    pad0: f32,
    pad1: f32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var cube_texture: texture_2d<f32>;
@group(0) @binding(2) var cube_sampler: sampler;

struct VertexIn {
    @location(0) position: vec3f,
    @location(1) uv: vec2f,
};

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) shade: f32,
};

@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    let cy = cos(uniforms.angle);
    let sy = sin(uniforms.angle);
    let cx = cos(0.45);
    let sx = sin(0.45);

    var p = input.position;
    let x0 = p.x * cy + p.z * sy;
    let z0 = -p.x * sy + p.z * cy;
    let y0 = p.y * cx - z0 * sx;
    let z1 = p.y * sx + z0 * cx;

    var out: VertexOut;
    out.position = vec4f(x0 * 1.25 / uniforms.aspect, y0 * 1.25, 0.5 - z1 * 0.35, 1.0);
    out.uv = input.uv;
    out.shade = clamp(0.72 - z1 * 0.32, 0.35, 1.0);
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4f {
    let texel = textureSample(cube_texture, cube_sampler, input.uv);
    return vec4f(texel.rgb * input.shade, 1.0);
}
)wgsl";

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    wgpu::ShaderModuleDescriptor shader_desc{};
    shader_desc.label = "Pulp Renderer3D hardcoded cube shader";
    shader_desc.nextInChain = &wgsl;
    auto shader = device.CreateShaderModule(&shader_desc);
    if (!shader) {
        result.error = "Renderer3D: failed to create shader module";
        return result;
    }

    std::array<wgpu::VertexAttribute, 2> attributes{};
    attributes[0].format = wgpu::VertexFormat::Float32x3;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = wgpu::VertexFormat::Float32x2;
    attributes[1].offset = sizeof(float) * 3u;
    attributes[1].shaderLocation = 1;

    wgpu::VertexBufferLayout vertex_layout{};
    vertex_layout.arrayStride = sizeof(Vertex);
    vertex_layout.stepMode = wgpu::VertexStepMode::Vertex;
    vertex_layout.attributeCount = attributes.size();
    vertex_layout.attributes = attributes.data();

    wgpu::ColorTargetState color_target{};
    color_target.format = wgpu::TextureFormat::RGBA8Unorm;
    color_target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState fragment{};
    fragment.module = shader;
    fragment.entryPoint = "fs_main";
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    wgpu::DepthStencilState depth_stencil{};
    depth_stencil.format = wgpu::TextureFormat::Depth24Plus;
    depth_stencil.depthWriteEnabled = true;
    depth_stencil.depthCompare = wgpu::CompareFunction::Less;

    wgpu::RenderPipelineDescriptor pipeline_desc{};
    pipeline_desc.label = "Pulp Renderer3D hardcoded cube pipeline";
    pipeline_desc.layout = nullptr;
    pipeline_desc.vertex.module = shader;
    pipeline_desc.vertex.entryPoint = "vs_main";
    pipeline_desc.vertex.bufferCount = 1;
    pipeline_desc.vertex.buffers = &vertex_layout;
    pipeline_desc.fragment = &fragment;
    pipeline_desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;
    pipeline_desc.primitive.cullMode = wgpu::CullMode::None;
    pipeline_desc.depthStencil = &depth_stencil;
    pipeline_desc.multisample.count = 1;
    pipeline_desc.multisample.mask = ~0u;
    auto pipeline = device.CreateRenderPipeline(&pipeline_desc);
    if (!pipeline) {
        result.error = "Renderer3D: failed to create render pipeline";
        return result;
    }

    std::array<wgpu::BindGroupEntry, 3> bind_entries{};
    bind_entries[0].binding = 0;
    bind_entries[0].buffer = uniform_buffer;
    bind_entries[0].offset = 0;
    bind_entries[0].size = sizeof(Uniforms);
    bind_entries[1].binding = 1;
    bind_entries[1].textureView = texture_view;
    bind_entries[2].binding = 2;
    bind_entries[2].sampler = sampler;

    wgpu::BindGroupDescriptor bind_desc{};
    bind_desc.label = "Pulp Renderer3D hardcoded cube bind group";
    bind_desc.layout = pipeline.GetBindGroupLayout(0);
    bind_desc.entryCount = bind_entries.size();
    bind_desc.entries = bind_entries.data();
    auto bind_group = device.CreateBindGroup(&bind_desc);
    if (!bind_group) {
        result.error = "Renderer3D: failed to create bind group";
        return result;
    }

    wgpu::RenderPassColorAttachment color_attachment{};
    color_attachment.view = color_view;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    color_attachment.clearValue = {0.02, 0.025, 0.035, 1.0};

    wgpu::RenderPassDepthStencilAttachment depth_attachment{};
    depth_attachment.view = depth_view;
    depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.depthClearValue = 1.0f;

    wgpu::RenderPassDescriptor pass_desc{};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_attachment;
    pass_desc.depthStencilAttachment = &depth_attachment;

    const uint32_t bytes_per_pixel = 4;
    const uint32_t compact_bytes_per_row = config.width * bytes_per_pixel;
    const uint32_t padded_bytes_per_row = align_to(compact_bytes_per_row, 256);
    const uint64_t readback_size =
        static_cast<uint64_t>(padded_bytes_per_row) * config.height;
    wgpu::BufferDescriptor readback_desc{};
    readback_desc.label = "Pulp Renderer3D hardcoded cube readback";
    readback_desc.size = readback_size;
    readback_desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    auto readback_buffer = device.CreateBuffer(&readback_desc);
    if (!readback_buffer) {
        result.error = "Renderer3D: failed to allocate readback buffer";
        return result;
    }

    wgpu::CommandEncoderDescriptor encoder_desc{};
    auto encoder = device.CreateCommandEncoder(&encoder_desc);
    if (!encoder) {
        result.error = "Renderer3D: failed to create command encoder";
        return result;
    }
    auto pass = encoder.BeginRenderPass(&pass_desc);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bind_group);
    pass.SetVertexBuffer(0, vertex_buffer);
    pass.SetIndexBuffer(index_buffer, wgpu::IndexFormat::Uint16);
    pass.DrawIndexed(indices.size(), 1, 0, 0, 0);
    pass.End();

    wgpu::TexelCopyTextureInfo copy_src{};
    copy_src.texture = color_texture;
    copy_src.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferInfo copy_dst{};
    copy_dst.buffer = readback_buffer;
    copy_dst.layout.bytesPerRow = padded_bytes_per_row;
    copy_dst.layout.rowsPerImage = config.height;
    wgpu::Extent3D copy_size{config.width, config.height, 1};
    encoder.CopyTextureToBuffer(&copy_src, &copy_dst, &copy_size);

    auto command_buffer = encoder.Finish();
    queue.Submit(1, &command_buffer);
    result.command_submitted = true;

    if (!finish_frame_readback(ctx, result, readback_buffer, readback_size,
                               padded_bytes_per_row, compact_bytes_per_row)) {
        return result;
    }

    result.success = result.color_target_allocated &&
                     result.depth_target_allocated &&
                     result.vertex_buffer_uploaded &&
                     result.index_buffer_uploaded &&
                     result.uniform_buffer_uploaded &&
                     result.texture_uploaded &&
                     result.command_submitted &&
                     result.readback_completed &&
                     result.distinct_color_count > 1 &&
                     result.non_transparent_pixel_count > 0;
    if (!result.success && result.error.empty()) {
        result.error = "Renderer3D: hardcoded cube structural validation failed";
    }
    return result;
#endif
}

Scene3DRenderResult Renderer3D::render_scene_data(
    const pulp::scene::SceneData& scene,
    const SceneDataRenderConfig& config) {
    Scene3DRenderResult result;
    result.width = config.width;
    result.height = config.height;
    result.fallback_adapter_requested = config.force_fallback_adapter;
    result.null_backend_requested =
        config.backend_preference ==
        Renderer3DAdapterBackendPreference::null_backend;

    if (config.width == 0 || config.height == 0) {
        result.error = "Renderer3D: render size must be non-zero";
        return result;
    }

#if !defined(PULP_HAS_SKIA) || !defined(PULP_HAS_WEBGPU)
    result.error = "Renderer3D: built without Dawn/WebGPU";
    return result;
#else
    // The analyzed primitives point into render_scene's sampler storage, so the
    // normalized copy outlives every phase below.
    auto render_scene = scene;
    const auto analysis = analyze_scene_data(scene, render_scene, result);
    if (!analysis) return result;

    auto frame = acquire_dawn_frame(config, result,
                                    "Pulp Renderer3D SceneData color",
                                    "Pulp Renderer3D SceneData depth");
    if (!frame) return result;
    auto& ctx = *frame;

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kSceneShaderWgsl;
    wgpu::ShaderModuleDescriptor shader_desc{};
    shader_desc.label = "Pulp Renderer3D SceneData shader";
    shader_desc.nextInChain = &wgsl;
    auto shader = ctx.device.CreateShaderModule(&shader_desc);
    if (!shader) {
        result.error = "Renderer3D: failed to create shader module";
        return result;
    }

    // vertex_layout points at `attributes`, so both stay in this scope for as
    // long as the pipelines are built from them.
    const auto attributes = scene_vertex_attributes();
    wgpu::VertexBufferLayout vertex_layout{};
    vertex_layout.arrayStride = sizeof(SceneVertex);
    vertex_layout.stepMode = wgpu::VertexStepMode::Vertex;
    vertex_layout.attributeCount = attributes.size();
    vertex_layout.attributes = attributes.data();

    wgpu::DepthStencilState depth_stencil{};
    depth_stencil.format = wgpu::TextureFormat::Depth24Plus;
    depth_stencil.depthWriteEnabled = true;
    depth_stencil.depthCompare = wgpu::CompareFunction::Less;

    std::vector<GpuScenePrimitive> gpu_primitives;
    if (!build_scene_gpu_primitives(ctx,
                                    config,
                                    *analysis,
                                    shader,
                                    vertex_layout,
                                    depth_stencil,
                                    gpu_primitives,
                                    result)) {
        return result;
    }

    auto readback = encode_scene_pass(ctx, config, gpu_primitives, result);
    if (!readback) return result;

    if (!finish_frame_readback(ctx, result, readback->buffer, readback->size,
                               readback->padded_bytes_per_row,
                               readback->compact_bytes_per_row)) {
        return result;
    }

    finalize_scene_result(result);
    return result;
#endif
}

} // namespace pulp::render
