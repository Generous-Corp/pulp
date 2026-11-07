#pragma once

/// @file gpu_capabilities.hpp
/// What the shared Dawn device can render, as plain data.
///
/// `GpuDeviceCapabilities` is filled from the device's `wgpu::Limits`, its
/// enabled features and the WebGPU format tables. `pick_tier()` reduces the
/// facts to a coarse rendering tier. Everything here is pure data and pure
/// functions: it names no Dawn type and needs no GPU, so a View can choose a
/// rendering path, and a test can cover that choice, without a device.

#include <cstdint>
#include <string_view>
#include <vector>

namespace pulp::render {

/// Texture formats the typed GPU API accepts. Names follow WebGPU
/// (`gpu_format_name(GpuFormat::rgba8_unorm) == "rgba8unorm"`).
enum class GpuFormat : std::uint8_t {
    undefined,
    r8_unorm,
    rg8_unorm,
    rgba8_unorm,
    rgba8_unorm_srgb,
    bgra8_unorm,
    bgra8_unorm_srgb,
    rgb10a2_unorm,
    rg11b10_ufloat,
    r16_float,
    rg16_float,
    rgba16_float,
    r32_float,
    rg32_float,
    rgba32_float,
    r32_uint,
    rgba8_uint,
    depth16_unorm,
    depth24_plus,
    depth24_plus_stencil8,
    depth32_float,
};

/// Every format above except `undefined`, in declaration order.
const std::vector<GpuFormat>& all_gpu_formats();

/// WebGPU spelling of a format ("rgba8unorm"), or "undefined".
std::string_view gpu_format_name(GpuFormat format);

/// Bytes per texel for color formats, 0 for depth/stencil and `undefined`
/// (depth texels have no defined CPU layout and cannot be read back).
std::uint32_t gpu_format_bytes_per_pixel(GpuFormat format);

/// True for depth and depth/stencil formats.
bool gpu_format_is_depth(GpuFormat format);

/// Optional device features that change what a format can do.
struct GpuDeviceFeatures {
    /// `rg11b10ufloat-renderable`
    bool rg11b10_ufloat_renderable = false;
    /// `float32-filterable`
    bool float32_filterable = false;
    /// `float32-blendable`
    bool float32_blendable = false;
    /// `bgra8unorm-storage`
    bool bgra8_unorm_storage = false;
    /// `timestamp-query`
    bool timestamp_query = false;
    /// `shader-f16`
    bool shader_f16 = false;
};

/// What one format supports on a device.
struct GpuFormatSupport {
    GpuFormat format = GpuFormat::undefined;
    /// Usable as a render-pass color (or depth) attachment.
    bool renderable = false;
    /// Blending is allowed when it is a color attachment.
    bool blendable = false;
    /// 4x multisampling is allowed. WebGPU guarantees exactly 1 and 4.
    bool multisample = false;
    /// Usable as a write-only storage texture.
    bool storage = false;
    /// Sampled with a filtering sampler.
    bool filterable = false;
};

/// The WebGPU format table for `format`, adjusted for `features`. Pure.
GpuFormatSupport gpu_format_support(GpuFormat format, const GpuDeviceFeatures& features);

/// Adapter class as Dawn reports it.
enum class GpuAdapterKind : std::uint8_t { unknown, integrated, discrete, cpu };

/// The facts `pick_tier()` reads. A test can build one by hand.
struct GpuDeviceFacts {
    bool available = false;
    /// Dawn's Null backend: validates API calls, draws nothing.
    bool null_backend = false;
    GpuAdapterKind adapter = GpuAdapterKind::unknown;
    std::uint32_t max_texture_dimension_2d = 0;
    std::uint32_t max_color_attachments = 0;
    std::uint32_t max_storage_textures_per_shader_stage = 0;
    std::uint32_t max_compute_invocations_per_workgroup = 0;
    std::uint64_t max_buffer_size = 0;
    GpuDeviceFeatures features;
};

/// Rendering capabilities of the shared device.
struct GpuDeviceCapabilities {
    GpuDeviceFacts facts;
    std::uint32_t max_texture_dimension_1d = 0;
    std::uint32_t max_texture_dimension_3d = 0;
    std::uint32_t max_texture_array_layers = 0;
    std::uint32_t max_bind_groups = 0;
    std::uint32_t max_bindings_per_bind_group = 0;
    std::uint32_t max_sampled_textures_per_shader_stage = 0;
    std::uint32_t max_samplers_per_shader_stage = 0;
    std::uint32_t max_uniform_buffers_per_shader_stage = 0;
    std::uint32_t max_storage_buffers_per_shader_stage = 0;
    std::uint64_t max_uniform_buffer_binding_size = 0;
    std::uint64_t max_storage_buffer_binding_size = 0;
    std::uint32_t max_vertex_buffers = 0;
    std::uint32_t max_vertex_attributes = 0;
    std::uint32_t max_color_attachment_bytes_per_sample = 0;
    std::uint32_t max_compute_workgroup_size_x = 0;
    std::uint32_t max_compute_workgroup_size_y = 0;
    std::uint32_t max_compute_workgroup_size_z = 0;
    std::uint32_t max_compute_workgroups_per_dimension = 0;
    std::uint32_t min_uniform_buffer_offset_alignment = 0;
    std::uint32_t min_storage_buffer_offset_alignment = 0;
    /// One entry per `all_gpu_formats()` entry, same order.
    std::vector<GpuFormatSupport> formats;

    /// Support for one format; an all-false record for an unknown format.
    GpuFormatSupport support(GpuFormat format) const;
    /// Formats usable as color attachments (depth formats excluded).
    std::vector<GpuFormat> renderable_color_formats() const;
    /// Formats usable as write-only storage textures.
    std::vector<GpuFormat> storage_formats() const;
    /// Sample counts a render target of `format` may use: {1} or {1, 4}.
    std::vector<std::uint32_t> sample_counts(GpuFormat format) const;
};

/// Fill the format table of `caps` from `caps.facts.features`. Pure.
void fill_format_table(GpuDeviceCapabilities& caps);

/// Coarse rendering tier for choosing a visual path.
///
///   none      no device, or the Null backend: render nothing on the GPU.
///   basic     a CPU (software) adapter, or a device below the `standard`
///             limits: keep passes small, avoid MSAA and float targets.
///   standard  a hardware adapter with at least 4 color attachments,
///             4 storage textures per stage, 256 compute invocations per
///             workgroup and 8192-texel 2D textures (the WebGPU defaults).
///   high      `standard` on a discrete adapter, or with 16384-texel
///             textures, 8 color attachments and float32 blending.
enum class GpuRenderTier : std::uint8_t { none, basic, standard, high };

std::string_view gpu_render_tier_name(GpuRenderTier tier);

/// Pure: the same facts always give the same tier.
GpuRenderTier pick_tier(const GpuDeviceFacts& facts);

} // namespace pulp::render
