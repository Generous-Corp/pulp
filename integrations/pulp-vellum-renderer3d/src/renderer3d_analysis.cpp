#include "renderer3d_analysis.hpp"

#include "renderer3d_materials.hpp"
#include "renderer3d_scene.hpp"

#include <string>

namespace pulp::vellum_renderer3d::detail {

// Copy the scene-wide feature rollup onto the public flat-bool result. Sticky
// OR (rather than plain assignment) so the copy is order-independent and cannot
// clear a flag another stage may already have recorded.
void apply_scene_feature_flags(Scene3DRenderResult& result, const SceneFeatureFlags& flags) {
    result.texture_decoded = result.texture_decoded || flags.texture_decoded;
    result.fallback_texture_used = result.fallback_texture_used || flags.fallback_texture_used;
    result.texture_sampler_applied =
        result.texture_sampler_applied || flags.texture_sampler_applied;
    result.texture_sampler_clamp_s =
        result.texture_sampler_clamp_s || flags.texture_sampler_clamp_s;
    result.texture_sampler_clamp_t =
        result.texture_sampler_clamp_t || flags.texture_sampler_clamp_t;
    result.texture_sampler_linear = result.texture_sampler_linear || flags.texture_sampler_linear;
    result.texture_mipmap_filter_downgraded =
        result.texture_mipmap_filter_downgraded || flags.texture_mipmap_filter_downgraded;
    result.base_color_transform_applied =
        result.base_color_transform_applied || flags.base_color_transform_applied;
    result.base_color_texcoord1_used =
        result.base_color_texcoord1_used || flags.base_color_texcoord1_used;
    result.base_color_factor_applied =
        result.base_color_factor_applied || flags.base_color_factor_applied;
    result.unlit_material_applied = result.unlit_material_applied || flags.unlit_material_applied;
    result.alpha_mask_applied = result.alpha_mask_applied || flags.alpha_mask_applied;
    result.alpha_blend_applied = result.alpha_blend_applied || flags.alpha_blend_applied;
    result.vertex_color_applied = result.vertex_color_applied || flags.vertex_color_applied;
    result.geometry_normals_applied =
        result.geometry_normals_applied || flags.geometry_normals_applied;
    result.metallic_roughness_factor_applied =
        result.metallic_roughness_factor_applied || flags.metallic_roughness_factor_applied;
    result.metallic_roughness_texture_applied =
        result.metallic_roughness_texture_applied || flags.metallic_roughness_texture_applied;
    result.double_sided_material_applied =
        result.double_sided_material_applied || flags.double_sided_material_applied;
    result.emissive_factor_applied =
        result.emissive_factor_applied || flags.emissive_factor_applied;
    result.emissive_strength_applied =
        result.emissive_strength_applied || flags.emissive_strength_applied;
    result.emissive_texture_applied =
        result.emissive_texture_applied || flags.emissive_texture_applied;
    result.tangent_attributes_available =
        result.tangent_attributes_available || flags.tangent_attributes_available;
    result.tangent_attributes_derived =
        result.tangent_attributes_derived || flags.tangent_attributes_derived;
    result.normal_texture_applied = result.normal_texture_applied || flags.normal_texture_applied;
    result.normal_scale_applied = result.normal_scale_applied || flags.normal_scale_applied;
    result.metallic_roughness_texture_deferred =
        result.metallic_roughness_texture_deferred || flags.metallic_roughness_texture_deferred;
    result.normal_texture_deferred =
        result.normal_texture_deferred || flags.normal_texture_deferred;
    result.normal_scale_deferred = result.normal_scale_deferred || flags.normal_scale_deferred;
    result.occlusion_texture_applied =
        result.occlusion_texture_applied || flags.occlusion_texture_applied;
    result.occlusion_strength_applied =
        result.occlusion_strength_applied || flags.occlusion_strength_applied;
    result.occlusion_texture_deferred =
        result.occlusion_texture_deferred || flags.occlusion_texture_deferred;
    result.occlusion_strength_deferred =
        result.occlusion_strength_deferred || flags.occlusion_strength_deferred;
    result.emissive_texture_deferred =
        result.emissive_texture_deferred || flags.emissive_texture_deferred;
    result.non_base_color_texture_transform_applied =
        result.non_base_color_texture_transform_applied ||
        flags.non_base_color_texture_transform_applied;
    result.non_base_color_texcoord1_used =
        result.non_base_color_texcoord1_used || flags.non_base_color_texcoord1_used;
    result.non_base_color_texture_transform_deferred =
        result.non_base_color_texture_transform_deferred ||
        flags.non_base_color_texture_transform_deferred;
    result.non_base_color_texcoord1_deferred =
        result.non_base_color_texcoord1_deferred || flags.non_base_color_texcoord1_deferred;
    result.advanced_material_extension_deferred =
        result.advanced_material_extension_deferred || flags.advanced_material_extension_deferred;
}

// Normalize the scene on the CPU and record every scene-level result flag:
// initial animation pose, lights, camera, deferred-feature reporting, and the
// per-primitive feature rollup. `scene` is the caller's untouched input (the
// deferred-animation and unsupported-feature reports describe it); `render_scene`
// is the mutable copy the pose is applied to and the primitives point into.
// Returns nullopt (result.error set) when the scene has no renderable primitive.
std::optional<SceneAnalysis> analyze_scene_data(const pulp::scene::SceneData& scene,
                                                pulp::scene::SceneData& render_scene,
                                                Scene3DRenderResult& result) {
    result.transform_animation_initial_pose_applied = apply_initial_animation_pose(render_scene);

    SceneAnalysis analysis;
    std::string scene_error;
    analysis.primitives = collect_renderable_primitives(render_scene, &analysis.normalization,
                                                        &analysis.transformed_nodes, &scene_error);
    if (analysis.primitives.empty()) {
        result.error =
            scene_error.empty() ? "Renderer3D: SceneData has no renderable primitive" : scene_error;
        return std::nullopt;
    }
    result.scene_data_consumed = true;
    result.primitive_count = static_cast<uint32_t>(analysis.primitives.size());

    analysis.directional_light = first_directional_light(render_scene, analysis.transformed_nodes);
    result.directional_light_applied = analysis.directional_light.applied;
    result.directional_light_transform_applied = analysis.directional_light.transform_applied;
    result.light_node_transform_deferred = has_deferred_light_node_transform(render_scene);
    const auto punctual_lights = deferred_punctual_lights(render_scene);
    analysis.point_light =
        first_point_light(render_scene, analysis.transformed_nodes, analysis.normalization);
    analysis.spot_light =
        first_spot_light(render_scene, analysis.transformed_nodes, analysis.normalization);
    result.point_light_applied = analysis.point_light.applied;
    result.point_light_deferred = punctual_lights.point && !analysis.point_light.applied;
    result.spot_light_applied = analysis.spot_light.applied;
    result.spot_light_deferred = punctual_lights.spot && !analysis.spot_light.applied;
    result.punctual_light_range_applied =
        analysis.point_light.range_applied || analysis.spot_light.range_applied;
    result.punctual_light_range_deferred =
        punctual_lights.point_range_count > (analysis.point_light.range_applied ? 1u : 0u) ||
        punctual_lights.spot_range_count > (analysis.spot_light.range_applied ? 1u : 0u);
    result.spot_light_cone_deferred = punctual_lights.spot_cone && !analysis.spot_light.applied;

    analysis.camera_projection =
        first_camera_projection(render_scene, analysis.transformed_nodes, analysis.normalization);
    result.perspective_camera_applied = analysis.camera_projection.perspective_applied;
    result.orthographic_camera_applied = analysis.camera_projection.orthographic_applied;
    result.camera_node_translation_applied = analysis.camera_projection.node_translation_applied;
    result.camera_node_rotation_applied = analysis.camera_projection.node_rotation_applied;
    result.camera_aspect_ratio_applied = analysis.camera_projection.aspect_ratio_applied;
    result.camera_depth_range_applied = analysis.camera_projection.depth_range_applied;
    const auto camera_metadata = deferred_camera_metadata(render_scene);
    result.camera_aspect_ratio_deferred =
        camera_metadata.aspect_ratio && !analysis.camera_projection.aspect_ratio_applied;
    result.camera_depth_range_deferred =
        has_deferred_camera_depth_range(render_scene, analysis.camera_projection.camera_index);
    result.camera_node_transform_deferred = has_deferred_camera_node_transform(render_scene);

    result.transform_animation_deferred = has_transform_animation(scene);
    const auto unsupported_features = deferred_unsupported_features(scene);
    result.skinning_deferred = unsupported_features.skinning;
    result.morph_target_deferred = unsupported_features.morph_target;
    result.gpu_instancing_deferred = unsupported_features.gpu_instancing;

    SceneFeatureFlags scene_flags;
    for (const auto& primitive : analysis.primitives) {
        scene_flags.merge(SceneFeatureFlags::from_primitive(primitive));
    }
    apply_scene_feature_flags(result, scene_flags);
    return analysis;
}

} // namespace pulp::vellum_renderer3d::detail
