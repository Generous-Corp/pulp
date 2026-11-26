#include "renderer3d_scene.hpp"

#include <pulp/scene/render_packet.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulp::vellum_renderer3d::detail {

bool node_has_non_identity_trs(const pulp::scene::NodeData& node) {
    constexpr float epsilon = 0.00001f;
    auto differs = [](float value, float expected) { return std::abs(value - expected) > epsilon; };
    return differs(node.translation[0], 0.0f) || differs(node.translation[1], 0.0f) ||
           differs(node.translation[2], 0.0f) || differs(node.rotation[0], 0.0f) ||
           differs(node.rotation[1], 0.0f) || differs(node.rotation[2], 0.0f) ||
           differs(node.rotation[3], 1.0f) || differs(node.scale[0], 1.0f) ||
           differs(node.scale[1], 1.0f) || differs(node.scale[2], 1.0f);
}

float dot3(float ax, float ay, float az, float bx, float by, float bz) {
    return ax * bx + ay * by + az * bz;
}

float length3(float x, float y, float z) {
    return std::sqrt(dot3(x, y, z, x, y, z));
}

void cross3(float ax, float ay, float az, float bx, float by, float bz, float& out_x, float& out_y,
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
    if (std::abs(node.matrix[3]) > epsilon || std::abs(node.matrix[7]) > epsilon ||
        std::abs(node.matrix[11]) > epsilon || std::abs(node.matrix[15] - 1.0f) > epsilon) {
        return false;
    }

    const float right_length = length3(node.matrix[0], node.matrix[1], node.matrix[2]);
    const float up_length = length3(node.matrix[4], node.matrix[5], node.matrix[6]);
    const float depth_length = length3(node.matrix[8], node.matrix[9], node.matrix[10]);
    if (std::abs(right_length - 1.0f) > epsilon || std::abs(up_length - 1.0f) > epsilon ||
        std::abs(depth_length - 1.0f) > epsilon) {
        return false;
    }

    return std::abs(dot3(node.matrix[0], node.matrix[1], node.matrix[2], node.matrix[4],
                         node.matrix[5], node.matrix[6])) <= epsilon &&
           std::abs(dot3(node.matrix[0], node.matrix[1], node.matrix[2], node.matrix[8],
                         node.matrix[9], node.matrix[10])) <= epsilon &&
           std::abs(dot3(node.matrix[4], node.matrix[5], node.matrix[6], node.matrix[8],
                         node.matrix[9], node.matrix[10])) <= epsilon;
}

bool node_has_deferred_camera_transform(const pulp::scene::NodeData& node) {
    constexpr float epsilon = 0.00001f;
    return !matrix_is_rigid_transform(node) || std::abs(node.scale[0] - 1.0f) > epsilon ||
           std::abs(node.scale[1] - 1.0f) > epsilon || std::abs(node.scale[2] - 1.0f) > epsilon;
}

bool node_has_deferred_light_transform(const pulp::scene::NodeData& node) {
    return node_has_deferred_camera_transform(node);
}

bool has_deferred_light_node_transform(const pulp::scene::SceneData& scene) {
    for (const auto& node : scene.nodes) {
        if (pulp::scene::is_valid_scene_index(node.light, scene.lights.size()) &&
            node_has_deferred_light_transform(node)) {
            return true;
        }
    }
    return false;
}

bool has_deferred_camera_node_transform(const pulp::scene::SceneData& scene) {
    for (const auto& node : scene.nodes) {
        if (pulp::scene::is_valid_scene_index(node.camera, scene.cameras.size()) &&
            node_has_deferred_camera_transform(node)) {
            return true;
        }
    }
    return false;
}

DeferredCameraMetadata deferred_camera_metadata(const pulp::scene::SceneData& scene) {
    auto scan_camera = [&scene](uint32_t camera_index, DeferredCameraMetadata& out) {
        if (!pulp::scene::is_valid_scene_index(camera_index, scene.cameras.size())) {
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

bool has_deferred_camera_depth_range(const pulp::scene::SceneData& scene,
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

pulp::scene::TransformedPoint transform_direction(const pulp::scene::Mat4& transform, float x,
                                                  float y, float z);

CpuDirectionalLight
first_directional_light(const pulp::scene::SceneData& scene,
                        const std::vector<pulp::scene::TransformedNode>& transformed_nodes) {
    auto light_from_index = [&scene](uint32_t light_index) -> std::optional<CpuDirectionalLight> {
        if (!pulp::scene::is_valid_scene_index(light_index, scene.lights.size())) {
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
        if (!pulp::scene::is_valid_scene_index(transformed.node, scene.nodes.size())) {
            continue;
        }
        const auto& node = scene.nodes[transformed.node];
        if (auto light = light_from_index(node.light)) {
            auto direction = transform_direction(transformed.world_transform, 0.0f, 0.0f, 1.0f);
            light->direction[0] = direction.x;
            light->direction[1] = direction.y;
            light->direction[2] = direction.z;
            normalize_direction(light->direction[0], light->direction[1], light->direction[2]);
            light->transform_applied = node_has_non_identity_trs(node) || node.has_matrix_transform;
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

DeferredPunctualLights deferred_punctual_lights(const pulp::scene::SceneData& scene) {
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
            if (light.inner_cone_angle > 0.0f || light.outer_cone_angle != 0.7853982f) {
                out.spot_cone = true;
            }
        }
    }
    return out;
}

CpuPointLight first_point_light(const pulp::scene::SceneData& scene,
                                const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
                                const SceneNormalization& normalization) {
    for (const auto& transformed : transformed_nodes) {
        if (!pulp::scene::is_valid_scene_index(transformed.node, scene.nodes.size())) {
            continue;
        }
        const auto& node = scene.nodes[transformed.node];
        if (!pulp::scene::is_valid_scene_index(node.light, scene.lights.size())) {
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
            (transformed.world_transform.m[12] - normalization.center[0]) * normalization.scale;
        out.position[1] =
            (transformed.world_transform.m[13] - normalization.center[1]) * normalization.scale;
        out.position[2] =
            (transformed.world_transform.m[14] - normalization.center[2]) * normalization.scale;
        if (light.range > 0.0f) {
            out.range = light.range * normalization.scale;
            out.range_applied = true;
        }
        out.applied = true;
        return out;
    }
    return {};
}

CpuSpotLight first_spot_light(const pulp::scene::SceneData& scene,
                              const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
                              const SceneNormalization& normalization) {
    for (const auto& transformed : transformed_nodes) {
        if (!pulp::scene::is_valid_scene_index(transformed.node, scene.nodes.size())) {
            continue;
        }
        const auto& node = scene.nodes[transformed.node];
        if (!pulp::scene::is_valid_scene_index(node.light, scene.lights.size())) {
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
            (transformed.world_transform.m[12] - normalization.center[0]) * normalization.scale;
        out.position[1] =
            (transformed.world_transform.m[13] - normalization.center[1]) * normalization.scale;
        out.position[2] =
            (transformed.world_transform.m[14] - normalization.center[2]) * normalization.scale;
        auto direction = transform_direction(transformed.world_transform, 0.0f, 0.0f, -1.0f);
        out.direction[0] = direction.x;
        out.direction[1] = direction.y;
        out.direction[2] = direction.z;
        normalize_direction(out.direction[0], out.direction[1], out.direction[2]);
        const float inner = std::clamp(light.inner_cone_angle, 0.0f, light.outer_cone_angle);
        const float outer = std::clamp(light.outer_cone_angle, inner, 1.5707964f);
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

CpuCameraProjection
first_camera_projection(const pulp::scene::SceneData& scene,
                        const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
                        const SceneNormalization& normalization) {
    auto apply_camera_node_offset = [&scene, &normalization](
                                        const pulp::scene::TransformedNode& transformed,
                                        CpuCameraProjection& projection) {
        if (!pulp::scene::is_valid_scene_index(transformed.node, scene.nodes.size())) {
            return;
        }
        const auto& node = scene.nodes[transformed.node];
        if (!pulp::scene::is_valid_scene_index(node.camera, scene.cameras.size())) {
            return;
        }
        if (node_has_deferred_camera_transform(node)) {
            return;
        }

        const float tx = transformed.world_transform.m[12];
        const float ty = transformed.world_transform.m[13];
        const float tz = transformed.world_transform.m[14];
        projection.camera_offset[0] = (tx - normalization.center[0]) * normalization.scale;
        projection.camera_offset[1] = (ty - normalization.center[1]) * normalization.scale;
        projection.camera_offset[2] = (tz - normalization.center[2]) * normalization.scale;
        constexpr float epsilon = 0.00001f;
        projection.node_translation_applied = std::abs(projection.camera_offset[0]) > epsilon ||
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
        normalize_direction(projection.camera_right[0], projection.camera_right[1],
                            projection.camera_right[2]);
        normalize_direction(projection.camera_up[0], projection.camera_up[1],
                            projection.camera_up[2]);
        normalize_direction(projection.camera_depth[0], projection.camera_depth[1],
                            projection.camera_depth[2]);
        projection.node_rotation_applied = std::abs(projection.camera_right[0] - 1.0f) > epsilon ||
                                           std::abs(projection.camera_right[1]) > epsilon ||
                                           std::abs(projection.camera_right[2]) > epsilon ||
                                           std::abs(projection.camera_up[0]) > epsilon ||
                                           std::abs(projection.camera_up[1] - 1.0f) > epsilon ||
                                           std::abs(projection.camera_up[2]) > epsilon ||
                                           std::abs(projection.camera_depth[0]) > epsilon ||
                                           std::abs(projection.camera_depth[1]) > epsilon ||
                                           std::abs(projection.camera_depth[2] - 1.0f) > epsilon;
    };

    auto camera_from_index = [&scene](uint32_t camera_index) -> std::optional<CpuCameraProjection> {
        if (!pulp::scene::is_valid_scene_index(camera_index, scene.cameras.size())) {
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
        if (camera.projection == pulp::scene::CameraData::Projection::perspective) {
            const float yfov = camera.yfov > 0.0f ? camera.yfov : 0.9f;
            out.projection_scale = 0.6f / std::max(std::tan(yfov * 0.5f), 0.05f);
            out.perspective_applied = true;
            return out;
        }
        if (camera.projection == pulp::scene::CameraData::Projection::orthographic) {
            const float ymag = camera.ymag > 0.0f ? camera.ymag : 2.0f;
            out.projection_scale = 0.95f / std::max(ymag, 0.05f);
            out.orthographic_applied = true;
            return out;
        }
        return std::nullopt;
    };

    for (const auto& transformed : transformed_nodes) {
        if (!pulp::scene::is_valid_scene_index(transformed.node, scene.nodes.size())) {
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
            if (!pulp::scene::is_valid_scene_index(channel.node, scene.nodes.size()) ||
                !pulp::scene::is_valid_scene_index(channel.sampler, animation.samplers.size())) {
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

DeferredUnsupportedFeatures deferred_unsupported_features(const pulp::scene::SceneData& scene) {
    DeferredUnsupportedFeatures out;
    for (const auto& feature : scene.unsupported_features) {
        if (feature.feature == "Skinning") {
            out.skinning = true;
        } else if (feature.feature == "MorphTargets" || feature.feature == "MorphWeights") {
            out.morph_target = true;
        } else if (feature.feature == "GpuInstancing") {
            out.gpu_instancing = true;
        }
    }
    return out;
}

pulp::scene::TransformedPoint transform_direction(const pulp::scene::Mat4& transform, float x,
                                                  float y, float z) {
    return pulp::scene::TransformedPoint{
        transform.m[0] * x + transform.m[4] * y + transform.m[8] * z,
        transform.m[1] * x + transform.m[5] * y + transform.m[9] * z,
        transform.m[2] * x + transform.m[6] * y + transform.m[10] * z,
    };
}

} // namespace pulp::vellum_renderer3d::detail
