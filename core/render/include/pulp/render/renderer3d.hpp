#pragma once

#include <pulp/render/headless_surface.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::scene {
struct SceneData;
}

namespace pulp::render {

enum class Renderer3DAdapterBackendPreference {
    default_backend,
    null_backend,
};

struct HardcodedCubeRenderConfig {
    uint32_t width = 256;
    uint32_t height = 256;
    bool force_fallback_adapter = false;
    Renderer3DAdapterBackendPreference backend_preference =
        Renderer3DAdapterBackendPreference::default_backend;
};

// What one offscreen Renderer3D render produced: the stage flags every entry
// point records, the adapter it ran on, the frame census, and the frame itself.
struct Scene3DRenderResult {
    bool success = false;
    bool gpu_available = false;
    bool scene_data_consumed = false;
    bool depth_target_allocated = false;
    bool color_target_allocated = false;
    bool vertex_buffer_uploaded = false;
    bool index_buffer_uploaded = false;
    bool uniform_buffer_uploaded = false;
    bool texture_uploaded = false;
    bool texture_decoded = false;
    bool fallback_texture_used = false;
    bool base_color_texture_srgb_applied = false;
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
    bool alpha_blend_depth_write_disabled = false;
    bool alpha_blend_sorted = false;
    bool vertex_color_applied = false;
    bool geometry_normals_applied = false;
    bool metallic_roughness_factor_applied = false;
    bool metallic_roughness_texture_applied = false;
    bool double_sided_material_applied = false;
    bool emissive_factor_applied = false;
    bool emissive_strength_applied = false;
    bool emissive_texture_applied = false;
    bool directional_light_applied = false;
    bool directional_light_transform_applied = false;
    bool light_node_transform_deferred = false;
    bool point_light_applied = false;
    bool point_light_deferred = false;
    bool spot_light_applied = false;
    bool spot_light_deferred = false;
    bool punctual_light_range_applied = false;
    bool punctual_light_range_deferred = false;
    bool spot_light_cone_deferred = false;
    bool perspective_camera_applied = false;
    bool orthographic_camera_applied = false;
    bool camera_node_translation_applied = false;
    bool camera_node_rotation_applied = false;
    bool camera_aspect_ratio_applied = false;
    bool camera_aspect_ratio_deferred = false;
    bool camera_depth_range_applied = false;
    bool camera_depth_range_deferred = false;
    bool camera_node_transform_deferred = false;
    bool transform_animation_initial_pose_applied = false;
    bool transform_animation_deferred = false;
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
    bool skinning_deferred = false;
    bool morph_target_deferred = false;
    bool gpu_instancing_deferred = false;
    bool command_submitted = false;
    bool readback_completed = false;
    bool adapter_info_available = false;
    bool fallback_adapter_requested = false;
    bool null_backend_requested = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t primitive_count = 0;
    uint32_t pipeline_cache_entry_count = 0;
    uint32_t pipeline_cache_hit_count = 0;
    uint32_t distinct_color_count = 0;
    uint32_t non_transparent_pixel_count = 0;
    std::string adapter_backend;
    std::string adapter_backend_type;
    std::string adapter_name;
    std::string adapter_vendor;
    std::string adapter_architecture;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> png;
    std::string error;
};

// Both entry points report through the same result type. The two original
// per-entry-point names stay as aliases for source compatibility.
using HardcodedCubeRenderResult = Scene3DRenderResult;

struct SceneDataRenderConfig {
    uint32_t width = 256;
    uint32_t height = 256;
    bool force_fallback_adapter = false;
    Renderer3DAdapterBackendPreference backend_preference =
        Renderer3DAdapterBackendPreference::default_backend;
};

using SceneDataRenderResult = Scene3DRenderResult;

class Renderer3D {
public:
    // Spike B proof: native Dawn render path, no JS, no Three.js, no glTF.
    static Scene3DRenderResult render_hardcoded_textured_cube(
        const HardcodedCubeRenderConfig& config = {});

    // Join proof: render already-normalized CPU SceneData with no JS or glTF
    // types crossing the renderer boundary.
    static Scene3DRenderResult render_scene_data(
        const scene::SceneData& scene,
        const SceneDataRenderConfig& config = {});
};

} // namespace pulp::render
