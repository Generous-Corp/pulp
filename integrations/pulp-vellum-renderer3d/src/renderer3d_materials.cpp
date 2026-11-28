#include "renderer3d_materials.hpp"

#include "renderer3d_scene.hpp"

#include <pulp/scene/render_packet.hpp>

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::vellum_renderer3d::detail {

std::optional<CpuTexture> decode_texture_rgba(const pulp::scene::TextureData& texture) {
    if (texture.encoded_bytes.empty()) {
        return std::nullopt;
    }

    auto sk_data =
        SkData::MakeWithoutCopy(texture.encoded_bytes.data(), texture.encoded_bytes.size());
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image || image->width() <= 0 || image->height() <= 0) {
        return std::nullopt;
    }

    CpuTexture out;
    out.width = static_cast<uint32_t>(image->width());
    out.height = static_cast<uint32_t>(image->height());
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4u);
    auto info = SkImageInfo::Make(image->width(), image->height(), kRGBA_8888_SkColorType,
                                  kUnpremul_SkAlphaType);
    if (!image->readPixels(info, out.rgba.data(), static_cast<size_t>(out.width) * 4u, 0, 0)) {
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

wgpu::AddressMode address_mode_from_scene_wrap(pulp::scene::TextureSamplerData::Wrap wrap) {
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

wgpu::FilterMode filter_mode_from_scene_filter(pulp::scene::TextureSamplerData::Filter filter,
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

wgpu::MipmapFilterMode
mipmap_filter_mode_from_scene_filter(pulp::scene::TextureSamplerData::Filter filter) {
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

std::pair<float, float>
transformed_texture_uv(const pulp::scene::PrimitiveData& primitive, uint32_t texture_texcoord,
                       const pulp::scene::TextureTransformData& texture_transform,
                       size_t vertex_index, bool* transform_applied, bool* texcoord1_used,
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
    } else if (texcoord == 0u && primitive.texcoord0.size() >= (vertex_index + 1u) * 2u) {
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

std::pair<float, float> transformed_base_color_uv(const pulp::scene::PrimitiveData& primitive,
                                                  const pulp::scene::MaterialData* material,
                                                  size_t vertex_index, bool* transform_applied,
                                                  bool* texcoord1_used) {
    static const pulp::scene::TextureTransformData kDefaultTransform;
    bool unsupported = false;
    return transformed_texture_uv(primitive, material ? material->base_color_texcoord : 0u,
                                  material ? material->base_color_transform : kDefaultTransform,
                                  vertex_index, transform_applied, texcoord1_used, &unsupported);
}

bool texture_uv_route_supported(const pulp::scene::PrimitiveData& primitive,
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
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
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
        const float normal_dot_tangent = dot3(vertex.nx, vertex.ny, vertex.nz, tx, ty, tz);
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
        cross3(vertex.nx, vertex.ny, vertex.nz, tx, ty, tz, cross_x, cross_y, cross_z);
        const float handedness = dot3(cross_x, cross_y, cross_z, bitangent_sum[i][0],
                                      bitangent_sum[i][1], bitangent_sum[i][2]) < 0.0f
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
    const pulp::scene::SceneData& scene, SceneNormalization* normalization,
    std::vector<pulp::scene::TransformedNode>* transformed_nodes_out, std::string* error) {
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
        if (!pulp::scene::is_valid_scene_index(render_primitive.mesh, scene.meshes.size())) {
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
        if (pulp::scene::is_valid_scene_index(primitive.material, scene.materials.size())) {
            material = &scene.materials[primitive.material];
        }

        const size_t vertex_count = primitive.positions.size() / 3u;
        const bool has_geometry_normals = primitive.normals.size() >= vertex_count * 3u;
        CpuPrimitive draw;
        draw.vertices.resize(vertex_count);
        draw.geometry_normals_applied = has_geometry_normals;
        const bool has_tangent_attributes = primitive.tangents.size() >= vertex_count * 4u;
        draw.tangent_attributes_available = has_tangent_attributes;
        if (material != nullptr) {
            draw.base_color_factor[0] = material->base_color_factor[0];
            draw.base_color_factor[1] = material->base_color_factor[1];
            draw.base_color_factor[2] = material->base_color_factor[2];
            draw.base_color_factor[3] = material->base_color_factor[3];
            draw.base_color_factor_applied = true;
            draw.metallic_factor = std::clamp(material->metallic_factor, 0.0f, 1.0f);
            draw.roughness_factor = std::clamp(material->roughness_factor, 0.0f, 1.0f);
            draw.metallic_roughness_factor_applied = true;
            draw.normal_scale = std::clamp(material->normal_scale, 0.0f, 4.0f);
            draw.occlusion_strength = std::clamp(material->occlusion_strength, 0.0f, 1.0f);
            draw.emissive_factor[0] = material->emissive_factor[0] * material->emissive_strength;
            draw.emissive_factor[1] = material->emissive_factor[1] * material->emissive_strength;
            draw.emissive_factor[2] = material->emissive_factor[2] * material->emissive_strength;
            draw.emissive_factor_applied = draw.emissive_factor[0] > 0.0f ||
                                           draw.emissive_factor[1] > 0.0f ||
                                           draw.emissive_factor[2] > 0.0f;
            draw.emissive_strength_applied =
                material->emissive_strength != 1.0f &&
                (material->emissive_factor[0] > 0.0f || material->emissive_factor[1] > 0.0f ||
                 material->emissive_factor[2] > 0.0f);
            draw.unlit = material->unlit;
            draw.alpha_mask = material->alpha_mode == pulp::scene::MaterialData::AlphaMode::mask;
            draw.alpha_blend = material->alpha_mode == pulp::scene::MaterialData::AlphaMode::blend;
            draw.alpha_cutoff = material->alpha_cutoff;
            draw.double_sided = material->double_sided;
            draw.metallic_roughness_texture_deferred = pulp::scene::is_valid_scene_index(
                material->metallic_roughness_texture, scene.textures.size());
            draw.normal_texture_deferred =
                pulp::scene::is_valid_scene_index(material->normal_texture, scene.textures.size());
            draw.normal_scale_deferred =
                draw.normal_texture_deferred && material->normal_scale != 1.0f;
            draw.occlusion_texture_deferred = pulp::scene::is_valid_scene_index(
                material->occlusion_texture, scene.textures.size());
            draw.occlusion_strength_deferred =
                draw.occlusion_texture_deferred && material->occlusion_strength != 1.0f;
            draw.emissive_texture_deferred = pulp::scene::is_valid_scene_index(
                material->emissive_texture, scene.textures.size());
            draw.advanced_material_extension_deferred =
                !material->advanced_material_extensions.empty();
        }

        for (size_t i = 0; i < vertex_count; ++i) {
            const auto point = pulp::scene::transform_point(
                render_primitive.world_transform, primitive.positions[i * 3u + 0u],
                primitive.positions[i * 3u + 1u], primitive.positions[i * 3u + 2u]);
            draw.vertices[i].x = point.x;
            draw.vertices[i].y = point.y;
            draw.vertices[i].z = point.z;
            const auto uv = transformed_base_color_uv(primitive, material, i,
                                                      &draw.base_color_transform_applied,
                                                      &draw.base_color_texcoord1_used);
            draw.vertices[i].u = uv.first;
            draw.vertices[i].v = uv.second;
            if (material != nullptr) {
                const bool has_metallic_roughness_texture = pulp::scene::is_valid_scene_index(
                    material->metallic_roughness_texture, scene.textures.size());
                bool metallic_roughness_transform_applied = false;
                bool metallic_roughness_texcoord1_used = false;
                bool unsupported_metallic_roughness_texcoord = false;
                const auto metallic_roughness_uv = transformed_texture_uv(
                    primitive, material->metallic_roughness_texcoord,
                    material->metallic_roughness_transform, i,
                    &metallic_roughness_transform_applied, &metallic_roughness_texcoord1_used,
                    &unsupported_metallic_roughness_texcoord);
                draw.vertices[i].metallic_roughness_u = metallic_roughness_uv.first;
                draw.vertices[i].metallic_roughness_v = metallic_roughness_uv.second;
                draw.non_base_color_texture_transform_applied =
                    draw.non_base_color_texture_transform_applied ||
                    (has_metallic_roughness_texture && metallic_roughness_transform_applied);
                draw.non_base_color_texcoord1_used =
                    draw.non_base_color_texcoord1_used ||
                    (has_metallic_roughness_texture && metallic_roughness_texcoord1_used);
                draw.non_base_color_texcoord1_deferred = draw.non_base_color_texcoord1_deferred ||
                                                         unsupported_metallic_roughness_texcoord;

                const bool has_normal_texture = pulp::scene::is_valid_scene_index(
                    material->normal_texture, scene.textures.size());
                bool normal_transform_applied = false;
                bool normal_texcoord1_used = false;
                bool unsupported_normal_texcoord = false;
                const auto normal_uv =
                    transformed_texture_uv(primitive, material->normal_texcoord,
                                           material->normal_transform, i, &normal_transform_applied,
                                           &normal_texcoord1_used, &unsupported_normal_texcoord);
                draw.vertices[i].normal_u = normal_uv.first;
                draw.vertices[i].normal_v = normal_uv.second;
                draw.non_base_color_texture_transform_applied =
                    draw.non_base_color_texture_transform_applied ||
                    (has_normal_texture && normal_transform_applied);
                draw.non_base_color_texcoord1_used = draw.non_base_color_texcoord1_used ||
                                                     (has_normal_texture && normal_texcoord1_used);
                draw.non_base_color_texcoord1_deferred =
                    draw.non_base_color_texcoord1_deferred || unsupported_normal_texcoord;

                const bool has_occlusion_texture = pulp::scene::is_valid_scene_index(
                    material->occlusion_texture, scene.textures.size());
                bool occlusion_transform_applied = false;
                bool occlusion_texcoord1_used = false;
                bool unsupported_occlusion_texcoord = false;
                const auto occlusion_uv = transformed_texture_uv(
                    primitive, material->occlusion_texcoord, material->occlusion_transform, i,
                    &occlusion_transform_applied, &occlusion_texcoord1_used,
                    &unsupported_occlusion_texcoord);
                draw.vertices[i].occlusion_u = occlusion_uv.first;
                draw.vertices[i].occlusion_v = occlusion_uv.second;
                draw.non_base_color_texture_transform_applied =
                    draw.non_base_color_texture_transform_applied ||
                    (has_occlusion_texture && occlusion_transform_applied);
                draw.non_base_color_texcoord1_used =
                    draw.non_base_color_texcoord1_used ||
                    (has_occlusion_texture && occlusion_texcoord1_used);
                draw.non_base_color_texcoord1_deferred =
                    draw.non_base_color_texcoord1_deferred || unsupported_occlusion_texcoord;

                const bool has_emissive_texture = pulp::scene::is_valid_scene_index(
                    material->emissive_texture, scene.textures.size());
                bool emissive_transform_applied = false;
                bool emissive_texcoord1_used = false;
                bool unsupported_emissive_texcoord = false;
                const auto emissive_uv = transformed_texture_uv(
                    primitive, material->emissive_texcoord, material->emissive_transform, i,
                    &emissive_transform_applied, &emissive_texcoord1_used,
                    &unsupported_emissive_texcoord);
                draw.vertices[i].emissive_u = emissive_uv.first;
                draw.vertices[i].emissive_v = emissive_uv.second;
                draw.non_base_color_texture_transform_applied =
                    draw.non_base_color_texture_transform_applied ||
                    (has_emissive_texture && emissive_transform_applied);
                draw.non_base_color_texcoord1_used =
                    draw.non_base_color_texcoord1_used ||
                    (has_emissive_texture && emissive_texcoord1_used);
                draw.non_base_color_texcoord1_deferred =
                    draw.non_base_color_texcoord1_deferred || unsupported_emissive_texcoord;
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
                    render_primitive.world_transform, primitive.normals[i * 3u + 0u],
                    primitive.normals[i * 3u + 1u], primitive.normals[i * 3u + 2u]);
                draw.vertices[i].nx = normal.x;
                draw.vertices[i].ny = normal.y;
                draw.vertices[i].nz = normal.z;
                normalize_direction(draw.vertices[i].nx, draw.vertices[i].ny, draw.vertices[i].nz);
            }
            if (primitive.tangents.size() >= (i + 1u) * 4u) {
                const auto tangent = transform_direction(
                    render_primitive.world_transform, primitive.tangents[i * 4u + 0u],
                    primitive.tangents[i * 4u + 1u], primitive.tangents[i * 4u + 2u]);
                draw.vertices[i].tx = tangent.x;
                draw.vertices[i].ty = tangent.y;
                draw.vertices[i].tz = tangent.z;
                draw.vertices[i].tw = primitive.tangents[i * 4u + 3u];
                normalize_direction(draw.vertices[i].tx, draw.vertices[i].ty, draw.vertices[i].tz);
            }

            min_x = std::min(min_x, point.x);
            min_y = std::min(min_y, point.y);
            min_z = std::min(min_z, point.z);
            max_x = std::max(max_x, point.x);
            max_y = std::max(max_y, point.y);
            max_z = std::max(max_z, point.z);
        }

        draw.indices = primitive.indices;

        if (!draw.tangent_attributes_available && has_geometry_normals && material != nullptr &&
            pulp::scene::is_valid_scene_index(material->normal_texture, scene.textures.size()) &&
            texture_uv_route_supported(primitive, material->normal_texcoord,
                                       material->normal_transform) &&
            derive_tangents_from_positions_normals_uvs(draw.vertices, draw.indices)) {
            draw.tangent_attributes_available = true;
            draw.tangent_attributes_derived = true;
        }

        if (material && pulp::scene::is_valid_scene_index(material->base_color_texture,
                                                          scene.textures.size())) {
            if (auto decoded = decode_texture_rgba(scene.textures[material->base_color_texture])) {
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
            pulp::scene::is_valid_scene_index(material->normal_texture, scene.textures.size())) {
            const bool normal_uv_supported = texture_uv_route_supported(
                primitive, material->normal_texcoord, material->normal_transform);
            if (auto decoded = decode_texture_rgba(scene.textures[material->normal_texture])) {
                draw.normal_texture = std::move(*decoded);
                draw.normal_texture_applied =
                    draw.tangent_attributes_available && normal_uv_supported;
                draw.normal_scale_applied =
                    draw.normal_texture_applied && material->normal_scale != 1.0f;
                draw.normal_texture_deferred =
                    !draw.tangent_attributes_available || !normal_uv_supported;
                draw.normal_scale_deferred =
                    (!draw.tangent_attributes_available || !normal_uv_supported) &&
                    material->normal_scale != 1.0f;
            }
            if (pulp::scene::is_valid_scene_index(material->normal_sampler,
                                                  scene.texture_samplers.size())) {
                draw.normal_sampler = &scene.texture_samplers[material->normal_sampler];
            }
            if (!normal_uv_supported) {
                draw.normal_texture_deferred = true;
                draw.normal_scale_deferred = material->normal_scale != 1.0f;
                draw.non_base_color_texcoord1_deferred = true;
            }
        }
        if (draw.normal_texture.rgba.empty()) {
            draw.normal_texture = make_solid_texture(128, 128, 255, 255);
        }

        if (material && pulp::scene::is_valid_scene_index(material->metallic_roughness_texture,
                                                          scene.textures.size())) {
            const bool metallic_roughness_uv_supported =
                texture_uv_route_supported(primitive, material->metallic_roughness_texcoord,
                                           material->metallic_roughness_transform);
            if (auto decoded =
                    decode_texture_rgba(scene.textures[material->metallic_roughness_texture])) {
                draw.metallic_roughness_texture = std::move(*decoded);
                draw.metallic_roughness_texture_applied = metallic_roughness_uv_supported;
                draw.metallic_roughness_texture_deferred = !metallic_roughness_uv_supported;
            }
            if (pulp::scene::is_valid_scene_index(material->metallic_roughness_sampler,
                                                  scene.texture_samplers.size())) {
                draw.metallic_roughness_sampler =
                    &scene.texture_samplers[material->metallic_roughness_sampler];
            }
            if (!metallic_roughness_uv_supported) {
                draw.metallic_roughness_texture_deferred = true;
                draw.non_base_color_texcoord1_deferred = true;
            }
        }
        if (draw.metallic_roughness_texture.rgba.empty()) {
            draw.metallic_roughness_texture = make_solid_texture(255, 255, 255, 255);
        }

        if (material &&
            pulp::scene::is_valid_scene_index(material->occlusion_texture, scene.textures.size())) {
            const bool occlusion_uv_supported = texture_uv_route_supported(
                primitive, material->occlusion_texcoord, material->occlusion_transform);
            if (auto decoded = decode_texture_rgba(scene.textures[material->occlusion_texture])) {
                draw.occlusion_texture = std::move(*decoded);
                draw.occlusion_texture_applied = occlusion_uv_supported;
                draw.occlusion_strength_applied =
                    occlusion_uv_supported && material->occlusion_strength != 1.0f;
                draw.occlusion_texture_deferred = !occlusion_uv_supported;
                draw.occlusion_strength_deferred =
                    !occlusion_uv_supported && material->occlusion_strength != 1.0f;
            }
            if (pulp::scene::is_valid_scene_index(material->occlusion_sampler,
                                                  scene.texture_samplers.size())) {
                draw.occlusion_sampler = &scene.texture_samplers[material->occlusion_sampler];
            }
            if (!occlusion_uv_supported) {
                draw.occlusion_texture_deferred = true;
                draw.occlusion_strength_deferred = material->occlusion_strength != 1.0f;
                draw.non_base_color_texcoord1_deferred = true;
            }
        }
        if (draw.occlusion_texture.rgba.empty()) {
            draw.occlusion_texture = make_solid_texture(255, 255, 255, 255);
        }

        if (material &&
            pulp::scene::is_valid_scene_index(material->emissive_texture, scene.textures.size())) {
            const bool emissive_uv_supported = texture_uv_route_supported(
                primitive, material->emissive_texcoord, material->emissive_transform);
            if (auto decoded = decode_texture_rgba(scene.textures[material->emissive_texture])) {
                draw.emissive_texture = std::move(*decoded);
                draw.emissive_texture_applied = emissive_uv_supported;
                draw.emissive_texture_deferred = !emissive_uv_supported;
            }
            if (pulp::scene::is_valid_scene_index(material->emissive_sampler,
                                                  scene.texture_samplers.size())) {
                draw.emissive_sampler = &scene.texture_samplers[material->emissive_sampler];
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
                draw.alpha_sort_depth = depth / static_cast<float>(draw.vertices.size());
            }
        }
        return draws;
    }

    if (error) {
        *error = "Renderer3D: SceneData has no renderable indexed primitive";
    }
    return {};
}

} // namespace pulp::vellum_renderer3d::detail
