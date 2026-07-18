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

// A min-filter that samples across mip levels; single-level textures downgrade
// it (reported via texture_mipmap_filter_downgraded) and the sampler descriptor
// falls back to a non-mipmap mode.
inline bool scene_filter_requires_mipmaps(
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

// Scene-wide material / texture feature reporting, grouped out of the flat-bool
// blizzard on Scene3DRenderResult. One value is built per renderable primitive
// (from_primitive) and rolled up with merge(); the accumulated value is copied
// onto the public result. Every flag is sticky: a feature counts as present for
// the scene when any single primitive uses it.
struct SceneFeatureFlags {
    bool texture_decoded = false;
    bool fallback_texture_used = false;
    bool texture_sampler_applied = false;
    bool texture_sampler_clamp_s = false;
    bool texture_sampler_clamp_t = false;
    bool texture_sampler_linear = false;
    bool texture_mipmap_filter_downgraded = false;
    bool base_color_transform_applied = false;
    bool base_color_texcoord1_used = false;
    bool base_color_factor_applied = false;
    bool unlit_material_applied = false;
    bool alpha_mask_applied = false;
    bool alpha_blend_applied = false;
    bool vertex_color_applied = false;
    bool geometry_normals_applied = false;
    bool metallic_roughness_factor_applied = false;
    bool metallic_roughness_texture_applied = false;
    bool double_sided_material_applied = false;
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

    // Sticky OR: fold another primitive's flags in. A feature stays applied once
    // any primitive sets it.
    void merge(const SceneFeatureFlags& other) {
        texture_decoded = texture_decoded || other.texture_decoded;
        fallback_texture_used =
            fallback_texture_used || other.fallback_texture_used;
        texture_sampler_applied =
            texture_sampler_applied || other.texture_sampler_applied;
        texture_sampler_clamp_s =
            texture_sampler_clamp_s || other.texture_sampler_clamp_s;
        texture_sampler_clamp_t =
            texture_sampler_clamp_t || other.texture_sampler_clamp_t;
        texture_sampler_linear =
            texture_sampler_linear || other.texture_sampler_linear;
        texture_mipmap_filter_downgraded =
            texture_mipmap_filter_downgraded ||
            other.texture_mipmap_filter_downgraded;
        base_color_transform_applied =
            base_color_transform_applied || other.base_color_transform_applied;
        base_color_texcoord1_used =
            base_color_texcoord1_used || other.base_color_texcoord1_used;
        base_color_factor_applied =
            base_color_factor_applied || other.base_color_factor_applied;
        unlit_material_applied =
            unlit_material_applied || other.unlit_material_applied;
        alpha_mask_applied = alpha_mask_applied || other.alpha_mask_applied;
        alpha_blend_applied = alpha_blend_applied || other.alpha_blend_applied;
        vertex_color_applied =
            vertex_color_applied || other.vertex_color_applied;
        geometry_normals_applied =
            geometry_normals_applied || other.geometry_normals_applied;
        metallic_roughness_factor_applied =
            metallic_roughness_factor_applied ||
            other.metallic_roughness_factor_applied;
        metallic_roughness_texture_applied =
            metallic_roughness_texture_applied ||
            other.metallic_roughness_texture_applied;
        double_sided_material_applied =
            double_sided_material_applied ||
            other.double_sided_material_applied;
        emissive_factor_applied =
            emissive_factor_applied || other.emissive_factor_applied;
        emissive_strength_applied =
            emissive_strength_applied || other.emissive_strength_applied;
        emissive_texture_applied =
            emissive_texture_applied || other.emissive_texture_applied;
        tangent_attributes_available =
            tangent_attributes_available ||
            other.tangent_attributes_available;
        tangent_attributes_derived =
            tangent_attributes_derived || other.tangent_attributes_derived;
        normal_texture_applied =
            normal_texture_applied || other.normal_texture_applied;
        normal_scale_applied =
            normal_scale_applied || other.normal_scale_applied;
        metallic_roughness_texture_deferred =
            metallic_roughness_texture_deferred ||
            other.metallic_roughness_texture_deferred;
        normal_texture_deferred =
            normal_texture_deferred || other.normal_texture_deferred;
        normal_scale_deferred =
            normal_scale_deferred || other.normal_scale_deferred;
        occlusion_texture_applied =
            occlusion_texture_applied || other.occlusion_texture_applied;
        occlusion_strength_applied =
            occlusion_strength_applied || other.occlusion_strength_applied;
        occlusion_texture_deferred =
            occlusion_texture_deferred || other.occlusion_texture_deferred;
        occlusion_strength_deferred =
            occlusion_strength_deferred || other.occlusion_strength_deferred;
        emissive_texture_deferred =
            emissive_texture_deferred || other.emissive_texture_deferred;
        non_base_color_texture_transform_applied =
            non_base_color_texture_transform_applied ||
            other.non_base_color_texture_transform_applied;
        non_base_color_texcoord1_used =
            non_base_color_texcoord1_used ||
            other.non_base_color_texcoord1_used;
        non_base_color_texture_transform_deferred =
            non_base_color_texture_transform_deferred ||
            other.non_base_color_texture_transform_deferred;
        non_base_color_texcoord1_deferred =
            non_base_color_texcoord1_deferred ||
            other.non_base_color_texcoord1_deferred;
        advanced_material_extension_deferred =
            advanced_material_extension_deferred ||
            other.advanced_material_extension_deferred;
    }

    // Extract one primitive's material / texture feature flags. The
    // sampler/texture-derived flags fold several primitive fields onto one
    // scene flag; the rest are name-matched.
    static SceneFeatureFlags from_primitive(const CpuPrimitive& primitive) {
        SceneFeatureFlags flags;
        flags.texture_decoded =
            primitive.texture.decoded ||
            primitive.normal_texture.decoded ||
            primitive.metallic_roughness_texture.decoded ||
            primitive.occlusion_texture.decoded ||
            primitive.emissive_texture.decoded;
        flags.fallback_texture_used = primitive.texture.fallback;
        flags.texture_sampler_applied = primitive.sampler != nullptr;
        flags.base_color_transform_applied =
            primitive.base_color_transform_applied;
        flags.base_color_texcoord1_used = primitive.base_color_texcoord1_used;
        flags.base_color_factor_applied = primitive.base_color_factor_applied;
        flags.unlit_material_applied = primitive.unlit;
        flags.alpha_mask_applied = primitive.alpha_mask;
        flags.alpha_blend_applied = primitive.alpha_blend;
        flags.vertex_color_applied = primitive.vertex_color_applied;
        flags.geometry_normals_applied = primitive.geometry_normals_applied;
        flags.metallic_roughness_factor_applied =
            primitive.metallic_roughness_factor_applied;
        flags.metallic_roughness_texture_applied =
            primitive.metallic_roughness_texture_applied;
        flags.double_sided_material_applied = primitive.double_sided;
        flags.emissive_factor_applied = primitive.emissive_factor_applied;
        flags.emissive_strength_applied = primitive.emissive_strength_applied;
        flags.emissive_texture_applied = primitive.emissive_texture_applied;
        flags.tangent_attributes_available =
            primitive.tangent_attributes_available;
        flags.tangent_attributes_derived =
            primitive.tangent_attributes_derived;
        flags.normal_texture_applied = primitive.normal_texture_applied;
        flags.normal_scale_applied = primitive.normal_scale_applied;
        flags.metallic_roughness_texture_deferred =
            primitive.metallic_roughness_texture_deferred;
        flags.normal_texture_deferred = primitive.normal_texture_deferred;
        flags.normal_scale_deferred = primitive.normal_scale_deferred;
        flags.occlusion_texture_applied = primitive.occlusion_texture_applied;
        flags.occlusion_strength_applied =
            primitive.occlusion_strength_applied;
        flags.occlusion_texture_deferred =
            primitive.occlusion_texture_deferred;
        flags.occlusion_strength_deferred =
            primitive.occlusion_strength_deferred;
        flags.emissive_texture_deferred = primitive.emissive_texture_deferred;
        flags.non_base_color_texture_transform_applied =
            primitive.non_base_color_texture_transform_applied;
        flags.non_base_color_texcoord1_used =
            primitive.non_base_color_texcoord1_used;
        flags.non_base_color_texture_transform_deferred =
            primitive.non_base_color_texture_transform_deferred;
        flags.non_base_color_texcoord1_deferred =
            primitive.non_base_color_texcoord1_deferred;
        flags.advanced_material_extension_deferred =
            primitive.advanced_material_extension_deferred;
        if (primitive.sampler != nullptr) {
            using Wrap = pulp::scene::TextureSamplerData::Wrap;
            using Filter = pulp::scene::TextureSamplerData::Filter;
            flags.texture_sampler_clamp_s =
                primitive.sampler->wrap_s == Wrap::clamp_to_edge;
            flags.texture_sampler_clamp_t =
                primitive.sampler->wrap_t == Wrap::clamp_to_edge;
            flags.texture_sampler_linear =
                primitive.sampler->mag_filter == Filter::linear ||
                primitive.sampler->min_filter == Filter::linear ||
                primitive.sampler->min_filter ==
                    Filter::linear_mipmap_nearest ||
                primitive.sampler->min_filter == Filter::linear_mipmap_linear;
            flags.texture_mipmap_filter_downgraded =
                scene_filter_requires_mipmaps(primitive.sampler->min_filter);
        }
        return flags;
    }
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
