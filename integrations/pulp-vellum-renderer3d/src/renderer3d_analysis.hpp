#pragma once

// The scene-wide CPU analysis every later render stage reads, and its
// projection onto the public result flags.

#include "renderer3d_internal.hpp"

#include <pulp/vellum_renderer3d/renderer3d.hpp>

#include <pulp/scene/scene_data.hpp>
#include <pulp/scene/scene_graph.hpp>

#include <optional>
#include <vector>

namespace pulp::vellum_renderer3d::detail {

// The renderable primitives plus the single directional / point / spot light
// and camera the scene shader supports. The primitives hold sampler pointers
// into the caller's normalized scene, so that scene must outlive the analysis.
struct SceneAnalysis {
    std::vector<CpuPrimitive> primitives;
    std::vector<pulp::scene::TransformedNode> transformed_nodes;
    SceneNormalization normalization;
    CpuDirectionalLight directional_light;
    CpuPointLight point_light;
    CpuSpotLight spot_light;
    CpuCameraProjection camera_projection;
};

void apply_scene_feature_flags(Scene3DRenderResult& result, const SceneFeatureFlags& flags);

/// Applies the initial animation pose to `render_scene`, collects its
/// primitives and lights and camera, and records the matching result flags.
/// nullopt, with result.error set, when the scene has nothing to render.
std::optional<SceneAnalysis> analyze_scene_data(const pulp::scene::SceneData& scene,
                                                pulp::scene::SceneData& render_scene,
                                                Scene3DRenderResult& result);

} // namespace pulp::vellum_renderer3d::detail
