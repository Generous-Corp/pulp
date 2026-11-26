#pragma once

// The offscreen frame every Renderer3D entry point draws into: a Dawn device
// borrowed from gpu-runtime's GpuSurface, colour and depth targets, and the
// blocking readback that turns the colour target into RGBA, a frame census
// and a PNG. gpu-runtime creates and owns the device; this package only holds
// its handles for the length of one render.

#include <pulp/vellum_renderer3d/renderer3d.hpp>

#include <gpu_runtime/gpu_surface.hpp>

#include "webgpu/webgpu_cpp.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulp::vellum_renderer3d::detail {

using gpu_runtime::GpuSurface;

// WebGPU requires `bytesPerRow` of a texture-to-buffer copy to be a multiple
// of 256, so a readback buffer holds each row padded to that alignment and
// the CPU copy drops the padding again.
inline constexpr std::uint32_t kCopyBytesPerRowAlignment = 256;

constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return ((value + alignment - 1u) / alignment) * alignment;
}

struct PaddedRowLayout {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t compact_bytes_per_row = 0;
    std::uint32_t padded_bytes_per_row = 0;
    /// Size of the readback buffer: every row padded.
    std::uint64_t buffer_size = 0;
};

constexpr PaddedRowLayout padded_row_layout(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t bytes_per_pixel) {
    PaddedRowLayout layout;
    layout.width = width;
    layout.height = height;
    layout.compact_bytes_per_row = width * bytes_per_pixel;
    layout.padded_bytes_per_row = align_up(layout.compact_bytes_per_row, kCopyBytesPerRowAlignment);
    layout.buffer_size = static_cast<std::uint64_t>(layout.padded_bytes_per_row) * height;
    return layout;
}

/// Copy `layout.height` padded rows from `padded` into a tightly packed
/// buffer. `padded` must hold `layout.buffer_size` bytes.
inline std::vector<std::uint8_t> compact_padded_rows(const std::uint8_t* padded,
                                                     const PaddedRowLayout& layout) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(layout.compact_bytes_per_row) *
                                  layout.height);
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * layout.compact_bytes_per_row,
                    padded + static_cast<std::size_t>(y) * layout.padded_bytes_per_row,
                    layout.compact_bytes_per_row);
    }
    return out;
}

/// Map `buffer` (which holds `layout.buffer_size` bytes of padded rows) for
/// reading, de-pad it and unmap it. Returns nullopt with `error` set when the
/// map fails, is cancelled, times out or yields a null range. Not real-time
/// safe: it spins the instance's event loop until the map resolves.
std::optional<std::vector<std::uint8_t>> read_padded_buffer(const wgpu::Instance& instance,
                                                            const wgpu::Buffer& buffer,
                                                            const PaddedRowLayout& layout,
                                                            std::chrono::milliseconds timeout,
                                                            std::string& error);

/// Encode tightly packed, unpremultiplied RGBA8 as a PNG. Empty, with `error`
/// set, when the buffer is smaller than `width * height * 4` or encoding fails.
std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t>& rgba, std::uint32_t width,
                                     std::uint32_t height, std::string& error);

GpuSurface::AdapterBackendPreference
map_backend_preference(Renderer3DAdapterBackendPreference preference);

void record_adapter_info(const GpuSurface& gpu, Scene3DRenderResult& result);

// Owns the Dawn frame resources every offscreen Renderer3D entry point needs:
// the GpuSurface plus its device/queue/instance handles and the color + depth
// render targets. Held by value so the device outlives the render.
struct FrameContext {
    std::unique_ptr<GpuSurface> gpu;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Instance instance;
    wgpu::Texture color_texture;
    wgpu::TextureView color_view;
    wgpu::Texture depth_texture;
    wgpu::TextureView depth_view;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FrameRequest {
    uint32_t width = 0;
    uint32_t height = 0;
    bool force_fallback_adapter = false;
    Renderer3DAdapterBackendPreference backend_preference =
        Renderer3DAdapterBackendPreference::default_backend;
};

// Create + initialize an offscreen Dawn surface and its color/depth targets,
// recording the gpu_available / adapter-info / *_target_allocated result flags.
// `color_label` / `depth_label` name the two textures for GPU debugging.
// Returns nullopt (with result.error set) on any failure.
std::optional<FrameContext> acquire_dawn_frame(const FrameRequest& request,
                                               Scene3DRenderResult& result, const char* color_label,
                                               const char* depth_label);

template <typename Config>
std::optional<FrameContext> acquire_dawn_frame(const Config& config, Scene3DRenderResult& result,
                                               const char* color_label, const char* depth_label) {
    FrameRequest request;
    request.width = config.width;
    request.height = config.height;
    request.force_fallback_adapter = config.force_fallback_adapter;
    request.backend_preference = config.backend_preference;
    return acquire_dawn_frame(request, result, color_label, depth_label);
}

// Blocking readback of the rendered color target into result.rgba: waits for
// the MapAsync to resolve (spinning the Dawn event loop on ctx.instance),
// de-pads each row, tallies the distinct-color / non-transparent-pixel census,
// and PNG-encodes the frame. Returns false (result.error set) on map timeout, a
// null mapping, or PNG-encode failure. Not real-time safe.
bool finish_frame_readback(FrameContext& ctx, Scene3DRenderResult& result,
                           const wgpu::Buffer& readback_buffer, const PaddedRowLayout& layout);

} // namespace pulp::vellum_renderer3d::detail
