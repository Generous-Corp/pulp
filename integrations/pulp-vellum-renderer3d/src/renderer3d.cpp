#include <pulp/vellum_renderer3d/renderer3d.hpp>

#include "renderer3d_frame.hpp"

#include "webgpu/webgpu_cpp.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace pulp::vellum_renderer3d {

bool Renderer3D::readback_holds_frame(const Scene3DRenderResult& result) {
    return result.distinct_color_count > 1 && result.non_transparent_pixel_count > 0 &&
           result.adapter_backend_type != "Null";
}

Scene3DRenderResult
Renderer3D::render_hardcoded_textured_cube(const HardcodedCubeRenderConfig& config) {
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

    auto frame = detail::acquire_dawn_frame(config, result, "Pulp Renderer3D hardcoded cube color",
                                            "Pulp Renderer3D hardcoded cube depth");
    if (!frame)
        return result;
    auto& ctx = *frame;
    auto& device = ctx.device;
    auto& queue = ctx.queue;
    auto& color_texture = ctx.color_texture;
    auto& color_view = ctx.color_view;
    auto& depth_view = ctx.depth_view;

    struct Vertex {
        float x, y, z;
        float u, v;
    };

    const std::array<Vertex, 24> vertices = {{
        {-0.5f, -0.5f, 0.5f, 0.0f, 1.0f},  {0.5f, -0.5f, 0.5f, 1.0f, 1.0f},
        {0.5f, 0.5f, 0.5f, 1.0f, 0.0f},    {-0.5f, 0.5f, 0.5f, 0.0f, 0.0f},
        {0.5f, -0.5f, -0.5f, 0.0f, 1.0f},  {-0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 0.5f, -0.5f, 1.0f, 0.0f},  {0.5f, 0.5f, -0.5f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, 1.0f}, {-0.5f, -0.5f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 0.5f, 0.5f, 1.0f, 0.0f},   {-0.5f, 0.5f, -0.5f, 0.0f, 0.0f},
        {0.5f, -0.5f, 0.5f, 0.0f, 1.0f},   {0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        {0.5f, 0.5f, -0.5f, 1.0f, 0.0f},   {0.5f, 0.5f, 0.5f, 0.0f, 0.0f},
        {-0.5f, 0.5f, 0.5f, 0.0f, 1.0f},   {0.5f, 0.5f, 0.5f, 1.0f, 1.0f},
        {0.5f, 0.5f, -0.5f, 1.0f, 0.0f},   {-0.5f, 0.5f, -0.5f, 0.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, 1.0f}, {0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        {0.5f, -0.5f, 0.5f, 1.0f, 0.0f},   {-0.5f, -0.5f, 0.5f, 0.0f, 0.0f},
    }};

    const std::array<uint16_t, 36> indices = {{
        0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
    }};

    struct Uniforms {
        float angle;
        float aspect;
        float pad0;
        float pad1;
    };
    const Uniforms uniforms{
        0.65f, static_cast<float>(config.width) / static_cast<float>(config.height), 0.0f, 0.0f};

    wgpu::BufferDescriptor vertex_desc{};
    vertex_desc.label = "Pulp Renderer3D cube vertices";
    vertex_desc.size = sizeof(vertices);
    vertex_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    auto vertex_buffer = device.CreateBuffer(&vertex_desc);
    if (!vertex_buffer) {
        result.error = "Renderer3D: failed to allocate vertex buffer";
        return result;
    }
    queue.WriteBuffer(vertex_buffer, 0, vertices.data(), sizeof(vertices));
    result.vertex_buffer_uploaded = true;

    wgpu::BufferDescriptor index_desc{};
    index_desc.label = "Pulp Renderer3D cube indices";
    index_desc.size = sizeof(indices);
    index_desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    auto index_buffer = device.CreateBuffer(&index_desc);
    if (!index_buffer) {
        result.error = "Renderer3D: failed to allocate index buffer";
        return result;
    }
    queue.WriteBuffer(index_buffer, 0, indices.data(), sizeof(indices));
    result.index_buffer_uploaded = true;

    wgpu::BufferDescriptor uniform_desc{};
    uniform_desc.label = "Pulp Renderer3D cube uniforms";
    uniform_desc.size = sizeof(Uniforms);
    uniform_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    auto uniform_buffer = device.CreateBuffer(&uniform_desc);
    if (!uniform_buffer) {
        result.error = "Renderer3D: failed to allocate uniform buffer";
        return result;
    }
    queue.WriteBuffer(uniform_buffer, 0, &uniforms, sizeof(uniforms));
    result.uniform_buffer_uploaded = true;

    const std::array<uint8_t, 4 * 4 * 4> texture_bytes = {{
        255, 40,  40,  255, 40,  255, 80, 255, 255, 40,  40,  255, 40,  255, 80, 255,
        50,  100, 255, 255, 255, 235, 40, 255, 50,  100, 255, 255, 255, 235, 40, 255,
        255, 40,  40,  255, 40,  255, 80, 255, 255, 40,  40,  255, 40,  255, 80, 255,
        50,  100, 255, 255, 255, 235, 40, 255, 50,  100, 255, 255, 255, 235, 40, 255,
    }};

    wgpu::TextureDescriptor texture_desc{};
    texture_desc.label = "Pulp Renderer3D cube texture";
    texture_desc.dimension = wgpu::TextureDimension::e2D;
    texture_desc.size = {4, 4, 1};
    texture_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    texture_desc.mipLevelCount = 1;
    texture_desc.sampleCount = 1;
    texture_desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    auto texture = device.CreateTexture(&texture_desc);
    if (!texture) {
        result.error = "Renderer3D: failed to allocate texture";
        return result;
    }
    wgpu::TexelCopyTextureInfo texture_dst{};
    texture_dst.texture = texture;
    texture_dst.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferLayout texture_layout{};
    texture_layout.bytesPerRow = 4 * 4;
    texture_layout.rowsPerImage = 4;
    wgpu::Extent3D texture_size{4, 4, 1};
    queue.WriteTexture(&texture_dst, texture_bytes.data(), texture_bytes.size(), &texture_layout,
                       &texture_size);
    result.texture_uploaded = true;
    auto texture_view = texture.CreateView();
    if (!texture_view) {
        result.error = "Renderer3D: failed to create texture view";
        return result;
    }

    wgpu::SamplerDescriptor sampler_desc{};
    sampler_desc.label = "Pulp Renderer3D cube sampler";
    sampler_desc.addressModeU = wgpu::AddressMode::Repeat;
    sampler_desc.addressModeV = wgpu::AddressMode::Repeat;
    sampler_desc.magFilter = wgpu::FilterMode::Nearest;
    sampler_desc.minFilter = wgpu::FilterMode::Nearest;
    auto sampler = device.CreateSampler(&sampler_desc);
    if (!sampler) {
        result.error = "Renderer3D: failed to create sampler";
        return result;
    }

    static constexpr const char* kShader = R"wgsl(
struct Uniforms {
    angle: f32,
    aspect: f32,
    pad0: f32,
    pad1: f32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var cube_texture: texture_2d<f32>;
@group(0) @binding(2) var cube_sampler: sampler;

struct VertexIn {
    @location(0) position: vec3f,
    @location(1) uv: vec2f,
};

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) shade: f32,
};

@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    let cy = cos(uniforms.angle);
    let sy = sin(uniforms.angle);
    let cx = cos(0.45);
    let sx = sin(0.45);

    var p = input.position;
    let x0 = p.x * cy + p.z * sy;
    let z0 = -p.x * sy + p.z * cy;
    let y0 = p.y * cx - z0 * sx;
    let z1 = p.y * sx + z0 * cx;

    var out: VertexOut;
    out.position = vec4f(x0 * 1.25 / uniforms.aspect, y0 * 1.25, 0.5 - z1 * 0.35, 1.0);
    out.uv = input.uv;
    out.shade = clamp(0.72 - z1 * 0.32, 0.35, 1.0);
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4f {
    let texel = textureSample(cube_texture, cube_sampler, input.uv);
    return vec4f(texel.rgb * input.shade, 1.0);
}
)wgsl";

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    wgpu::ShaderModuleDescriptor shader_desc{};
    shader_desc.label = "Pulp Renderer3D hardcoded cube shader";
    shader_desc.nextInChain = &wgsl;
    auto shader = device.CreateShaderModule(&shader_desc);
    if (!shader) {
        result.error = "Renderer3D: failed to create shader module";
        return result;
    }

    std::array<wgpu::VertexAttribute, 2> attributes{};
    attributes[0].format = wgpu::VertexFormat::Float32x3;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = wgpu::VertexFormat::Float32x2;
    attributes[1].offset = sizeof(float) * 3u;
    attributes[1].shaderLocation = 1;

    wgpu::VertexBufferLayout vertex_layout{};
    vertex_layout.arrayStride = sizeof(Vertex);
    vertex_layout.stepMode = wgpu::VertexStepMode::Vertex;
    vertex_layout.attributeCount = attributes.size();
    vertex_layout.attributes = attributes.data();

    wgpu::ColorTargetState color_target{};
    color_target.format = wgpu::TextureFormat::RGBA8Unorm;
    color_target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState fragment{};
    fragment.module = shader;
    fragment.entryPoint = "fs_main";
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    wgpu::DepthStencilState depth_stencil{};
    depth_stencil.format = wgpu::TextureFormat::Depth24Plus;
    depth_stencil.depthWriteEnabled = true;
    depth_stencil.depthCompare = wgpu::CompareFunction::Less;

    wgpu::RenderPipelineDescriptor pipeline_desc{};
    pipeline_desc.label = "Pulp Renderer3D hardcoded cube pipeline";
    pipeline_desc.layout = nullptr;
    pipeline_desc.vertex.module = shader;
    pipeline_desc.vertex.entryPoint = "vs_main";
    pipeline_desc.vertex.bufferCount = 1;
    pipeline_desc.vertex.buffers = &vertex_layout;
    pipeline_desc.fragment = &fragment;
    pipeline_desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;
    pipeline_desc.primitive.cullMode = wgpu::CullMode::None;
    pipeline_desc.depthStencil = &depth_stencil;
    pipeline_desc.multisample.count = 1;
    pipeline_desc.multisample.mask = ~0u;
    auto pipeline = device.CreateRenderPipeline(&pipeline_desc);
    if (!pipeline) {
        result.error = "Renderer3D: failed to create render pipeline";
        return result;
    }

    std::array<wgpu::BindGroupEntry, 3> bind_entries{};
    bind_entries[0].binding = 0;
    bind_entries[0].buffer = uniform_buffer;
    bind_entries[0].offset = 0;
    bind_entries[0].size = sizeof(Uniforms);
    bind_entries[1].binding = 1;
    bind_entries[1].textureView = texture_view;
    bind_entries[2].binding = 2;
    bind_entries[2].sampler = sampler;

    wgpu::BindGroupDescriptor bind_desc{};
    bind_desc.label = "Pulp Renderer3D hardcoded cube bind group";
    bind_desc.layout = pipeline.GetBindGroupLayout(0);
    bind_desc.entryCount = bind_entries.size();
    bind_desc.entries = bind_entries.data();
    auto bind_group = device.CreateBindGroup(&bind_desc);
    if (!bind_group) {
        result.error = "Renderer3D: failed to create bind group";
        return result;
    }

    wgpu::RenderPassColorAttachment color_attachment{};
    color_attachment.view = color_view;
    color_attachment.loadOp = wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    color_attachment.clearValue = {0.02, 0.025, 0.035, 1.0};

    wgpu::RenderPassDepthStencilAttachment depth_attachment{};
    depth_attachment.view = depth_view;
    depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.depthClearValue = 1.0f;

    wgpu::RenderPassDescriptor pass_desc{};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_attachment;
    pass_desc.depthStencilAttachment = &depth_attachment;

    const uint32_t bytes_per_pixel = 4;
    const auto readback_layout =
        detail::padded_row_layout(config.width, config.height, bytes_per_pixel);
    wgpu::BufferDescriptor readback_desc{};
    readback_desc.label = "Pulp Renderer3D hardcoded cube readback";
    readback_desc.size = readback_layout.buffer_size;
    readback_desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    auto readback_buffer = device.CreateBuffer(&readback_desc);
    if (!readback_buffer) {
        result.error = "Renderer3D: failed to allocate readback buffer";
        return result;
    }

    wgpu::CommandEncoderDescriptor encoder_desc{};
    auto encoder = device.CreateCommandEncoder(&encoder_desc);
    if (!encoder) {
        result.error = "Renderer3D: failed to create command encoder";
        return result;
    }
    auto pass = encoder.BeginRenderPass(&pass_desc);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bind_group);
    pass.SetVertexBuffer(0, vertex_buffer);
    pass.SetIndexBuffer(index_buffer, wgpu::IndexFormat::Uint16);
    pass.DrawIndexed(indices.size(), 1, 0, 0, 0);
    pass.End();

    wgpu::TexelCopyTextureInfo copy_src{};
    copy_src.texture = color_texture;
    copy_src.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferInfo copy_dst{};
    copy_dst.buffer = readback_buffer;
    copy_dst.layout.bytesPerRow = readback_layout.padded_bytes_per_row;
    copy_dst.layout.rowsPerImage = config.height;
    wgpu::Extent3D copy_size{config.width, config.height, 1};
    encoder.CopyTextureToBuffer(&copy_src, &copy_dst, &copy_size);

    auto command_buffer = encoder.Finish();
    queue.Submit(1, &command_buffer);
    result.command_submitted = true;

    if (!detail::finish_frame_readback(ctx, result, readback_buffer, readback_layout)) {
        return result;
    }

    result.success = result.color_target_allocated && result.depth_target_allocated &&
                     result.vertex_buffer_uploaded && result.index_buffer_uploaded &&
                     result.uniform_buffer_uploaded && result.texture_uploaded &&
                     result.command_submitted && result.readback_completed &&
                     readback_holds_frame(result);
    if (!result.success && result.error.empty()) {
        result.error = "Renderer3D: hardcoded cube structural validation failed";
    }
    return result;
}

} // namespace pulp::vellum_renderer3d
