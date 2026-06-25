#pragma once

#include <pulp/scene/scene_data.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::render {

struct SceneVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 1.0f;
    float tx = 1.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    float tw = 1.0f;
    float normal_u = 0.0f;
    float normal_v = 0.0f;
    float metallic_roughness_u = 0.0f;
    float metallic_roughness_v = 0.0f;
    float occlusion_u = 0.0f;
    float occlusion_v = 0.0f;
    float emissive_u = 0.0f;
    float emissive_v = 0.0f;
};

struct CpuTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    bool decoded = false;
    bool fallback = false;
};

struct CpuPrimitive {
    std::vector<SceneVertex> vertices;
    std::vector<uint32_t> indices;
    CpuTexture texture;
    CpuTexture normal_texture;
    CpuTexture metallic_roughness_texture;
    CpuTexture occlusion_texture;
    CpuTexture emissive_texture;
    const pulp::scene::TextureSamplerData* sampler = nullptr;
    const pulp::scene::TextureSamplerData* normal_sampler = nullptr;
    const pulp::scene::TextureSamplerData* metallic_roughness_sampler = nullptr;
    const pulp::scene::TextureSamplerData* occlusion_sampler = nullptr;
    const pulp::scene::TextureSamplerData* emissive_sampler = nullptr;
    float base_color_factor[4] = {0.85f, 0.35f, 0.20f, 1.0f};
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float emissive_factor[3] = {0.0f, 0.0f, 0.0f};
    bool base_color_transform_applied = false;
    bool base_color_texcoord1_used = false;
    bool base_color_factor_applied = false;
    bool unlit = false;
    bool alpha_mask = false;
    bool alpha_blend = false;
    float alpha_sort_depth = 0.0f;
    float alpha_cutoff = 0.5f;
    bool vertex_color_applied = false;
    bool geometry_normals_applied = false;
    bool metallic_roughness_factor_applied = false;
    bool metallic_roughness_texture_applied = false;
    bool double_sided = false;
    bool emissive_factor_applied = false;
    bool emissive_strength_applied = false;
    bool emissive_texture_applied = false;
    bool tangent_attributes_available = false;
    bool tangent_attributes_derived = false;
    bool normal_texture_applied = false;
    bool normal_scale_applied = false;
    bool metallic_roughness_texture_deferred = false;
    bool normal_texture_deferred = false;
    bool normal_scale_deferred = false;
    bool occlusion_texture_applied = false;
    bool occlusion_strength_applied = false;
    bool occlusion_texture_deferred = false;
    bool occlusion_strength_deferred = false;
    bool emissive_texture_deferred = false;
    bool non_base_color_texture_transform_applied = false;
    bool non_base_color_texcoord1_used = false;
    bool non_base_color_texture_transform_deferred = false;
    bool non_base_color_texcoord1_deferred = false;
    bool advanced_material_extension_deferred = false;
};

struct ScenePipelineKey {
    bool alpha_blend = false;
    bool double_sided = false;
};

inline bool operator==(const ScenePipelineKey& lhs,
                       const ScenePipelineKey& rhs) {
    return lhs.alpha_blend == rhs.alpha_blend &&
           lhs.double_sided == rhs.double_sided;
}

struct ScenePipelineKeyHash {
    std::size_t operator()(const ScenePipelineKey& key) const {
        return (key.alpha_blend ? 0x9e3779b97f4a7c15ull : 0ull) ^
               (key.double_sided ? 0xbf58476d1ce4e5b9ull : 0ull);
    }
};

struct CpuDirectionalLight {
    float color[3] = {1.0f, 1.0f, 1.0f};
    float direction[3] = {0.25f, 0.35f, 0.90f};
    bool applied = false;
    bool transform_applied = false;
};

struct CpuPointLight {
    float color[3] = {0.0f, 0.0f, 0.0f};
    float position[3] = {0.0f, 0.0f, 1.0f};
    float range = 0.0f;
    bool applied = false;
    bool range_applied = false;
};

struct CpuSpotLight {
    float color[3] = {0.0f, 0.0f, 0.0f};
    float position[3] = {0.0f, 0.0f, 1.0f};
    float direction[3] = {0.0f, 0.0f, -1.0f};
    float inner_cos = 1.0f;
    float outer_cos = 0.70710677f;
    float range = 0.0f;
    bool applied = false;
    bool range_applied = false;
};

struct CpuCameraProjection {
    uint32_t camera_index = pulp::scene::invalid_scene_index;
    float projection_scale = 1.0f;
    float aspect_ratio = 1.0f;
    float znear = 0.1f;
    float zfar = 100.0f;
    float camera_offset[3] = {0.0f, 0.0f, 0.0f};
    float camera_right[3] = {1.0f, 0.0f, 0.0f};
    float camera_up[3] = {0.0f, 1.0f, 0.0f};
    float camera_depth[3] = {0.0f, 0.0f, 1.0f};
    bool perspective_applied = false;
    bool orthographic_applied = false;
    bool node_translation_applied = false;
    bool node_rotation_applied = false;
    bool aspect_ratio_applied = false;
    bool depth_range_applied = false;

    bool applied() const {
        return perspective_applied || orthographic_applied;
    }
};

struct SceneNormalization {
    float center[3] = {0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
};

struct DeferredCameraMetadata {
    bool aspect_ratio = false;
    bool depth_range = false;
};

struct DeferredUnsupportedFeatures {
    bool skinning = false;
    bool morph_target = false;
    bool gpu_instancing = false;
};

} // namespace pulp::render
