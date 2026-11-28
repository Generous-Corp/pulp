#pragma once

// Scene-graph metadata the renderer reads before it draws: the single
// directional, point and spot light and the camera the scene shader supports,
// the initial animation pose, and the features it reports as deferred. CPU
// only; nothing here touches the GPU.

#include "renderer3d_internal.hpp"

#include <pulp/scene/scene_data.hpp>
#include <pulp/scene/scene_graph.hpp>

#include <cstdint>
#include <vector>

namespace pulp::vellum_renderer3d::detail {

struct DeferredPunctualLights {
    bool point = false;
    bool spot = false;
    uint32_t point_range_count = 0;
    uint32_t spot_range_count = 0;
    bool spot_cone = false;
};

float dot3(float ax, float ay, float az, float bx, float by, float bz);
float length3(float x, float y, float z);
void cross3(float ax, float ay, float az, float bx, float by, float bz, float& out_x, float& out_y,
            float& out_z);

bool node_has_non_identity_trs(const pulp::scene::NodeData& node);
bool matrix_is_rigid_transform(const pulp::scene::NodeData& node);
bool has_deferred_light_node_transform(const pulp::scene::SceneData& scene);
bool has_deferred_camera_node_transform(const pulp::scene::SceneData& scene);
DeferredCameraMetadata deferred_camera_metadata(const pulp::scene::SceneData& scene);
bool has_deferred_camera_depth_range(const pulp::scene::SceneData& scene,
                                     uint32_t consumed_camera_index);

/// Normalizes in place; a zero-length vector becomes +Z.
void normalize_direction(float& x, float& y, float& z);

/// The upper-left 3x3 of a column-major `transform` applied to a direction.
pulp::scene::TransformedPoint transform_direction(const pulp::scene::Mat4& transform, float x,
                                                  float y, float z);

CpuDirectionalLight
first_directional_light(const pulp::scene::SceneData& scene,
                        const std::vector<pulp::scene::TransformedNode>& transformed_nodes);
DeferredPunctualLights deferred_punctual_lights(const pulp::scene::SceneData& scene);
CpuPointLight first_point_light(const pulp::scene::SceneData& scene,
                                const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
                                const SceneNormalization& normalization);
CpuSpotLight first_spot_light(const pulp::scene::SceneData& scene,
                              const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
                              const SceneNormalization& normalization);
CpuCameraProjection
first_camera_projection(const pulp::scene::SceneData& scene,
                        const std::vector<pulp::scene::TransformedNode>& transformed_nodes,
                        const SceneNormalization& normalization);

bool has_transform_animation(const pulp::scene::SceneData& scene);
/// Applies each animation channel's first keyframe to its node; true when any
/// node changed.
bool apply_initial_animation_pose(pulp::scene::SceneData& scene);
DeferredUnsupportedFeatures deferred_unsupported_features(const pulp::scene::SceneData& scene);

} // namespace pulp::vellum_renderer3d::detail
