#include <pulp/vellum_renderer3d/renderer3d.hpp>

#include "renderer3d_analysis.hpp"
#include "renderer3d_frame.hpp"
#include "renderer3d_internal.hpp"
#include "renderer3d_materials.hpp"
#include "renderer3d_scene_wgsl.hpp"

#include <pulp/scene/scene_data.hpp>

#include "webgpu/webgpu_cpp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::vellum_renderer3d {

namespace {

using namespace detail;

// Per-primitive uniform block; layout mirrors the Uniforms struct in
// shaders/renderer3d_scene.wgsl and the two must be edited together.
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

// The scene shader is shaders/renderer3d_scene.wgsl, embedded and
// validated at build time by pulp_add_wgsl_shaders().
constexpr std::string_view kSceneShaderWgsl = shaders::renderer3d_scene_wgsl;

// Vertex attribute table for SceneVertex; offsets track the field order in
// renderer3d_internal.hpp and the @location table in renderer3d_scene.wgsl.
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

SceneUniforms make_scene_uniforms(const CpuPrimitive& primitive, const SceneAnalysis& analysis,
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
                : static_cast<float>(config.width) / static_cast<float>(config.height),
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
std::optional<GpuSceneTexture> upload_scene_texture(FrameContext& ctx, const CpuTexture& source,
                                                    wgpu::TextureFormat format, const char* label,
                                                    const char* kind, Scene3DRenderResult& result,
                                                    OnWritten&& on_written) {
    wgpu::TextureDescriptor texture_desc{};
    texture_desc.label = label;
    texture_desc.dimension = wgpu::TextureDimension::e2D;
    texture_desc.size = {source.width, source.height, 1};
    texture_desc.format = format;
    texture_desc.mipLevelCount = 1;
    texture_desc.sampleCount = 1;
    texture_desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;

    GpuSceneTexture uploaded;
    uploaded.texture = ctx.device.CreateTexture(&texture_desc);
    if (!uploaded.texture) {
        result.error = std::string("Renderer3D: failed to allocate ") + kind + "texture";
        return std::nullopt;
    }

    wgpu::TexelCopyTextureInfo texture_dst{};
    texture_dst.texture = uploaded.texture;
    texture_dst.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferLayout texture_layout{};
    texture_layout.bytesPerRow = source.width * 4u;
    texture_layout.rowsPerImage = source.height;
    wgpu::Extent3D texture_size{source.width, source.height, 1};
    ctx.queue.WriteTexture(&texture_dst, source.rgba.data(), source.rgba.size(), &texture_layout,
                           &texture_size);
    on_written();

    uploaded.view = uploaded.texture.CreateView();
    if (!uploaded.view) {
        result.error = std::string("Renderer3D: failed to create ") + kind + "texture view";
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
    desc.magFilter = filter_mode_from_scene_filter(sampler->mag_filter, wgpu::FilterMode::Nearest);
    desc.minFilter = filter_mode_from_scene_filter(sampler->min_filter, desc.magFilter);
    desc.mipmapFilter = scene_filter_requires_mipmaps(sampler->min_filter)
                            ? wgpu::MipmapFilterMode::Nearest
                            : mipmap_filter_mode_from_scene_filter(sampler->min_filter);
}

// `kind` names the sampler in the failure message and is empty for the base
// color sampler. Returns a null sampler (result.error set) on failure.
wgpu::Sampler create_scene_sampler(FrameContext& ctx, const wgpu::SamplerDescriptor& desc,
                                   const char* kind, Scene3DRenderResult& result) {
    auto sampler = ctx.device.CreateSampler(&desc);
    if (!sampler) {
        result.error = std::string("Renderer3D: failed to create ") + kind + "sampler";
    }
    return sampler;
}

// Build one GpuScenePrimitive per analyzed primitive: buffers, textures,
// samplers, a pipeline from a cache keyed on the blend / cull state, and the
// bind group. Draw order is settled here as well — opaque first, then the
// blended primitives back to front. Returns false (result.error set) on any GPU
// allocation failure, leaving the flags recorded so far in place.
bool build_scene_gpu_primitives(FrameContext& ctx, const SceneDataRenderConfig& config,
                                const SceneAnalysis& analysis, const wgpu::ShaderModule& shader,
                                const wgpu::VertexBufferLayout& vertex_layout,
                                const wgpu::DepthStencilState& depth_stencil,
                                std::vector<GpuScenePrimitive>& gpu_primitives,
                                Scene3DRenderResult& result) {
    gpu_primitives.reserve(analysis.primitives.size());
    std::unordered_map<ScenePipelineKey, wgpu::RenderPipeline, ScenePipelineKeyHash> pipeline_cache;

    for (const auto& primitive : analysis.primitives) {
        const SceneUniforms uniforms = make_scene_uniforms(primitive, analysis, config);

        GpuScenePrimitive gpu_primitive;
        gpu_primitive.index_count = static_cast<uint32_t>(primitive.indices.size());
        gpu_primitive.alpha_blend = primitive.alpha_blend;
        gpu_primitive.alpha_sort_depth = primitive.alpha_sort_depth;

        wgpu::BufferDescriptor vertex_desc{};
        vertex_desc.label = "Pulp Renderer3D SceneData vertices";
        vertex_desc.size = primitive.vertices.size() * sizeof(SceneVertex);
        vertex_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        gpu_primitive.vertex_buffer = ctx.device.CreateBuffer(&vertex_desc);
        if (!gpu_primitive.vertex_buffer) {
            result.error = "Renderer3D: failed to allocate vertex buffer";
            return false;
        }
        ctx.queue.WriteBuffer(gpu_primitive.vertex_buffer, 0, primitive.vertices.data(),
                              vertex_desc.size);
        result.vertex_buffer_uploaded = true;

        wgpu::BufferDescriptor index_desc{};
        index_desc.label = "Pulp Renderer3D SceneData indices";
        index_desc.size = primitive.indices.size() * sizeof(uint32_t);
        index_desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        gpu_primitive.index_buffer = ctx.device.CreateBuffer(&index_desc);
        if (!gpu_primitive.index_buffer) {
            result.error = "Renderer3D: failed to allocate index buffer";
            return false;
        }
        ctx.queue.WriteBuffer(gpu_primitive.index_buffer, 0, primitive.indices.data(),
                              index_desc.size);
        result.index_buffer_uploaded = true;

        wgpu::BufferDescriptor uniform_desc{};
        uniform_desc.label = "Pulp Renderer3D SceneData uniforms";
        uniform_desc.size = sizeof(SceneUniforms);
        uniform_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        gpu_primitive.uniform_buffer = ctx.device.CreateBuffer(&uniform_desc);
        if (!gpu_primitive.uniform_buffer) {
            result.error = "Renderer3D: failed to allocate uniform buffer";
            return false;
        }
        ctx.queue.WriteBuffer(gpu_primitive.uniform_buffer, 0, &uniforms, sizeof(uniforms));
        result.uniform_buffer_uploaded = true;

        auto base_color =
            upload_scene_texture(ctx, primitive.texture, wgpu::TextureFormat::RGBA8UnormSrgb,
                                 "Pulp Renderer3D SceneData texture", "", result, [&result] {
                                     result.texture_uploaded = true;
                                     result.base_color_texture_srgb_applied = true;
                                 });
        if (!base_color) {
            return false;
        }
        gpu_primitive.base_color = std::move(*base_color);

        auto normal = upload_scene_texture(
            ctx, primitive.normal_texture, wgpu::TextureFormat::RGBA8Unorm,
            "Pulp Renderer3D SceneData normal texture", "normal ", result, [] {});
        if (!normal) {
            return false;
        }
        gpu_primitive.normal = std::move(*normal);

        auto metallic_roughness = upload_scene_texture(
            ctx, primitive.metallic_roughness_texture, wgpu::TextureFormat::RGBA8Unorm,
            "Pulp Renderer3D SceneData metallic-roughness texture", "metallic-roughness ", result,
            [] {});
        if (!metallic_roughness) {
            return false;
        }
        gpu_primitive.metallic_roughness = std::move(*metallic_roughness);

        auto occlusion = upload_scene_texture(
            ctx, primitive.occlusion_texture, wgpu::TextureFormat::RGBA8Unorm,
            "Pulp Renderer3D SceneData occlusion texture", "occlusion ", result, [] {});
        if (!occlusion) {
            return false;
        }
        gpu_primitive.occlusion = std::move(*occlusion);

        auto emissive = upload_scene_texture(
            ctx, primitive.emissive_texture, wgpu::TextureFormat::RGBA8UnormSrgb,
            "Pulp Renderer3D SceneData emissive texture", "emissive ", result, [] {});
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
        gpu_primitive.sampler = create_scene_sampler(ctx, sampler_desc, "", result);
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
        apply_scene_sampler(metallic_roughness_sampler_desc, primitive.metallic_roughness_sampler);
        gpu_primitive.metallic_roughness_sampler = create_scene_sampler(
            ctx, metallic_roughness_sampler_desc, "metallic-roughness ", result);
        if (!gpu_primitive.metallic_roughness_sampler) {
            return false;
        }

        wgpu::SamplerDescriptor occlusion_sampler_desc = sampler_desc;
        occlusion_sampler_desc.label = "Pulp Renderer3D SceneData occlusion sampler";
        apply_scene_sampler(occlusion_sampler_desc, primitive.occlusion_sampler);
        gpu_primitive.occlusion_sampler =
            create_scene_sampler(ctx, occlusion_sampler_desc, "occlusion ", result);
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
                alpha_blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                alpha_blend.alpha.operation = wgpu::BlendOperation::Add;
                alpha_blend.alpha.srcFactor = wgpu::BlendFactor::One;
                alpha_blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
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
            pipeline_desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
            pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;
            pipeline_desc.primitive.cullMode =
                primitive.double_sided ? wgpu::CullMode::None : wgpu::CullMode::Back;
            pipeline_desc.depthStencil = &primitive_depth_stencil;
            pipeline_desc.multisample.count = 1;
            pipeline_desc.multisample.mask = ~0u;
            gpu_primitive.pipeline = ctx.device.CreateRenderPipeline(&pipeline_desc);
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

    result.pipeline_cache_entry_count = static_cast<uint32_t>(pipeline_cache.size());
    const auto blended_count = static_cast<size_t>(
        std::count_if(gpu_primitives.begin(), gpu_primitives.end(),
                      [](const GpuScenePrimitive& primitive) { return primitive.alpha_blend; }));
    std::stable_sort(gpu_primitives.begin(), gpu_primitives.end(),
                     [](const GpuScenePrimitive& lhs, const GpuScenePrimitive& rhs) {
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
    detail::PaddedRowLayout layout;
};

// Encode and submit the scene pass: clear, draw every primitive in the order
// build_scene_gpu_primitives settled, then copy the color target into a
// mappable readback buffer. Returns nullopt (result.error set) when a GPU
// object fails to allocate.
std::optional<SceneReadback> encode_scene_pass(FrameContext& ctx,
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
    readback.layout = detail::padded_row_layout(config.width, config.height, bytes_per_pixel);
    wgpu::BufferDescriptor readback_desc{};
    readback_desc.label = "Pulp Renderer3D SceneData readback";
    readback_desc.size = readback.layout.buffer_size;
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
        pass.SetIndexBuffer(gpu_primitive.index_buffer, wgpu::IndexFormat::Uint32);
        pass.DrawIndexed(gpu_primitive.index_count, 1, 0, 0, 0);
    }
    pass.End();

    wgpu::TexelCopyTextureInfo copy_src{};
    copy_src.texture = ctx.color_texture;
    copy_src.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferInfo copy_dst{};
    copy_dst.buffer = readback.buffer;
    copy_dst.layout.bytesPerRow = readback.layout.padded_bytes_per_row;
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
    result.success = result.scene_data_consumed && result.color_target_allocated &&
                     result.depth_target_allocated && result.vertex_buffer_uploaded &&
                     result.index_buffer_uploaded && result.uniform_buffer_uploaded &&
                     result.texture_uploaded && result.command_submitted &&
                     result.readback_completed && Renderer3D::readback_holds_frame(result);
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
        append_missing(result.non_transparent_pixel_count > 0, "non_transparent_pixel_count");
        append_missing(result.adapter_backend_type != "Null", "drawing_adapter");
    }
}

} // namespace

Scene3DRenderResult Renderer3D::render_scene_data(const pulp::scene::SceneData& scene,
                                                  const SceneDataRenderConfig& config) {
    Scene3DRenderResult result;
    result.width = config.width;
    result.height = config.height;
    result.fallback_adapter_requested = config.force_fallback_adapter;
    result.null_backend_requested =
        config.backend_preference == Renderer3DAdapterBackendPreference::null_backend;

    if (config.width == 0 || config.height == 0) {
        result.error = "Renderer3D: render size must be non-zero";
        return result;
    }

    // The analyzed primitives point into render_scene's sampler storage, so the
    // normalized copy outlives every phase below.
    auto render_scene = scene;
    const auto analysis = analyze_scene_data(scene, render_scene, result);
    if (!analysis)
        return result;

    auto frame = acquire_dawn_frame(config, result, "Pulp Renderer3D SceneData color",
                                    "Pulp Renderer3D SceneData depth");
    if (!frame)
        return result;
    auto& ctx = *frame;

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = wgpu::StringView(kSceneShaderWgsl.data(), kSceneShaderWgsl.size());
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
    if (!build_scene_gpu_primitives(ctx, config, *analysis, shader, vertex_layout, depth_stencil,
                                    gpu_primitives, result)) {
        return result;
    }

    auto readback = encode_scene_pass(ctx, config, gpu_primitives, result);
    if (!readback)
        return result;

    if (!finish_frame_readback(ctx, result, readback->buffer, readback->layout)) {
        return result;
    }

    finalize_scene_result(result);
    return result;
}

} // namespace pulp::vellum_renderer3d
