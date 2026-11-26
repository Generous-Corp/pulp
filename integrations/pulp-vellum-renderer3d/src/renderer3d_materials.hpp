#pragma once

// CPU material and texture projection: texture decoding (Skia), fallback and
// solid textures, sampler conversion, UV transforms, tangent derivation, and
// the conversion of a scene's renderable primitives into CPU draw records.

#include "renderer3d_internal.hpp"

#include <pulp/scene/scene_data.hpp>
#include <pulp/scene/scene_graph.hpp>

#include "webgpu/webgpu_cpp.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::vellum_renderer3d::detail {

/// Decodes the encoded image with Skia's codecs to unpremultiplied RGBA8;
/// nullopt when it does not decode.
std::optional<CpuTexture> decode_texture_rgba(const pulp::scene::TextureData& texture);
uint8_t color_factor_to_byte(float value);
/// A 4x4 checker the renderer draws when a texture does not decode.
CpuTexture make_fallback_texture();
CpuTexture make_solid_texture(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

wgpu::AddressMode address_mode_from_scene_wrap(pulp::scene::TextureSamplerData::Wrap wrap);
wgpu::FilterMode filter_mode_from_scene_filter(pulp::scene::TextureSamplerData::Filter filter,
                                               wgpu::FilterMode fallback);
wgpu::MipmapFilterMode
mipmap_filter_mode_from_scene_filter(pulp::scene::TextureSamplerData::Filter filter);

bool derive_tangents_from_positions_normals_uvs(std::vector<SceneVertex>& vertices,
                                                const std::vector<uint32_t>& indices);

/// The scene's renderable primitives as CPU draw records, normalized into the
/// renderer's view volume. Empty, with `error` set, when nothing renders.
std::vector<CpuPrimitive> collect_renderable_primitives(
    const pulp::scene::SceneData& scene, SceneNormalization* normalization,
    std::vector<pulp::scene::TransformedNode>* transformed_nodes_out, std::string* error);

} // namespace pulp::vellum_renderer3d::detail
