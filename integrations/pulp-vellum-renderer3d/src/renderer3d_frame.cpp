#include "renderer3d_frame.hpp"

#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"

#include <thread>
#include <unordered_set>
#include <utility>

namespace pulp::vellum_renderer3d {

uint64_t rgba_fingerprint(const Scene3DRenderResult& result) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t h = kOffset;
    // Dimensions first, so two all-zero frames of transposed sizes differ.
    const uint32_t dims[2] = {result.width, result.height};
    const auto* dim_bytes = reinterpret_cast<const uint8_t*>(dims);
    for (size_t i = 0; i < sizeof(dims); ++i) {
        h ^= static_cast<uint64_t>(dim_bytes[i]);
        h *= kPrime;
    }
    for (uint8_t b : result.rgba) {
        h ^= static_cast<uint64_t>(b);
        h *= kPrime;
    }
    return h;
}

namespace detail {

std::optional<std::vector<std::uint8_t>> read_padded_buffer(const wgpu::Instance& instance,
                                                            const wgpu::Buffer& buffer,
                                                            const PaddedRowLayout& layout,
                                                            std::chrono::milliseconds timeout,
                                                            std::string& error) {
    // The callback can outlive this frame if the map never resolves before the
    // deadline, so it writes into shared state rather than into locals.
    struct MapState {
        bool done = false;
        bool ok = false;
    };
    auto state = std::make_shared<MapState>();
    buffer.MapAsync(wgpu::MapMode::Read, 0, layout.buffer_size,
                    wgpu::CallbackMode::AllowProcessEvents,
                    [state](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        state->done = true;
                        state->ok = status == wgpu::MapAsyncStatus::Success;
                    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!state->done && std::chrono::steady_clock::now() < deadline) {
        instance.ProcessEvents();
        if (!state->done)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!state->done || !state->ok) {
        error = state->done ? "readback map failed" : "readback map timed out";
        return std::nullopt;
    }
    const auto* padded =
        static_cast<const std::uint8_t*>(buffer.GetConstMappedRange(0, layout.buffer_size));
    if (padded == nullptr) {
        error = "mapped readback returned null";
        buffer.Unmap();
        return std::nullopt;
    }
    auto pixels = compact_padded_rows(padded, layout);
    buffer.Unmap();
    return pixels;
}

std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t>& rgba, std::uint32_t width,
                                     std::uint32_t height, std::string& error) {
    if (rgba.empty() || width == 0 || height == 0) {
        error = "Renderer3D: PNG encode of an empty frame";
        return {};
    }
    const size_t need = static_cast<size_t>(width) * height * 4u;
    if (rgba.size() < need) {
        error = "Renderer3D: PNG encode buffer smaller than width*height*4";
        return {};
    }
    auto info = SkImageInfo::Make(static_cast<int>(width), static_cast<int>(height),
                                  kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkPixmap pixmap(info, rgba.data(), static_cast<size_t>(width) * 4u);
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png || png->size() == 0) {
        error = "Renderer3D: SkPngEncoder::Encode returned empty";
        return {};
    }
    std::vector<std::uint8_t> out(png->size());
    std::memcpy(out.data(), png->data(), png->size());
    return out;
}

GpuSurface::AdapterBackendPreference
map_backend_preference(Renderer3DAdapterBackendPreference preference) {
    switch (preference) {
    case Renderer3DAdapterBackendPreference::null_backend:
        return GpuSurface::AdapterBackendPreference::null_backend;
    case Renderer3DAdapterBackendPreference::vulkan:
        return GpuSurface::AdapterBackendPreference::vulkan;
    case Renderer3DAdapterBackendPreference::d3d12:
        return GpuSurface::AdapterBackendPreference::d3d12;
    case Renderer3DAdapterBackendPreference::default_backend:
        return GpuSurface::AdapterBackendPreference::default_backend;
    }
    return GpuSurface::AdapterBackendPreference::default_backend;
}

void record_adapter_info(const GpuSurface& gpu, Scene3DRenderResult& result) {
    const auto info = gpu.adapter_info();
    result.adapter_info_available = info.available;
    result.adapter_backend = info.backend;
    result.adapter_backend_type = info.backend_type;
    switch (info.adapter_type) {
    case GpuSurface::AdapterType::integrated_gpu:
        result.adapter_type = "integrated-gpu";
        break;
    case GpuSurface::AdapterType::discrete_gpu:
        result.adapter_type = "discrete-gpu";
        break;
    case GpuSurface::AdapterType::cpu:
        result.adapter_type = "cpu";
        break;
    default:
        result.adapter_type = "unknown";
        break;
    }
    result.adapter_name = info.name;
    result.adapter_vendor = info.vendor;
    result.adapter_architecture = info.architecture;
}

std::optional<FrameContext> acquire_dawn_frame(const FrameRequest& request,
                                               Scene3DRenderResult& result, const char* color_label,
                                               const char* depth_label) {
    FrameContext ctx;
    ctx.width = request.width;
    ctx.height = request.height;

    ctx.gpu = GpuSurface::create_dawn();
    if (!ctx.gpu) {
        result.error = "Renderer3D: failed to create Dawn GpuSurface";
        return std::nullopt;
    }

    GpuSurface::Config gpu_config;
    gpu_config.width = request.width;
    gpu_config.height = request.height;
    gpu_config.native_surface_handle = nullptr;
    gpu_config.force_fallback_adapter = request.force_fallback_adapter;
    gpu_config.backend_preference = map_backend_preference(request.backend_preference);
    if (!ctx.gpu->initialize(gpu_config)) {
        result.error = "Renderer3D: failed to initialize offscreen Dawn surface";
        return std::nullopt;
    }
    result.gpu_available = true;
    record_adapter_info(*ctx.gpu, result);

    auto* device_ptr = static_cast<wgpu::Device*>(ctx.gpu->dawn_device_handle());
    auto* queue_ptr = static_cast<wgpu::Queue*>(ctx.gpu->dawn_queue_handle());
    auto* instance_ptr = static_cast<wgpu::Instance*>(ctx.gpu->dawn_instance_handle());
    if (device_ptr == nullptr || queue_ptr == nullptr || instance_ptr == nullptr ||
        !(*device_ptr) || !(*queue_ptr) || !(*instance_ptr)) {
        result.error = "Renderer3D: missing Dawn device, queue, or instance";
        return std::nullopt;
    }
    ctx.device = *device_ptr;
    ctx.queue = *queue_ptr;
    ctx.instance = *instance_ptr;

    wgpu::TextureDescriptor color_desc{};
    color_desc.label = color_label;
    color_desc.dimension = wgpu::TextureDimension::e2D;
    color_desc.size = {request.width, request.height, 1};
    color_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    color_desc.mipLevelCount = 1;
    color_desc.sampleCount = 1;
    color_desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    ctx.color_texture = ctx.device.CreateTexture(&color_desc);
    if (!ctx.color_texture) {
        result.error = "Renderer3D: failed to allocate color target";
        return std::nullopt;
    }
    ctx.color_view = ctx.color_texture.CreateView();
    result.color_target_allocated = ctx.color_view != nullptr;
    if (!result.color_target_allocated) {
        result.error = "Renderer3D: failed to create color target view";
        return std::nullopt;
    }

    wgpu::TextureDescriptor depth_desc{};
    depth_desc.label = depth_label;
    depth_desc.dimension = wgpu::TextureDimension::e2D;
    depth_desc.size = {request.width, request.height, 1};
    depth_desc.format = wgpu::TextureFormat::Depth24Plus;
    depth_desc.mipLevelCount = 1;
    depth_desc.sampleCount = 1;
    depth_desc.usage = wgpu::TextureUsage::RenderAttachment;
    ctx.depth_texture = ctx.device.CreateTexture(&depth_desc);
    if (!ctx.depth_texture) {
        result.error = "Renderer3D: failed to allocate depth target";
        return std::nullopt;
    }
    ctx.depth_view = ctx.depth_texture.CreateView();
    result.depth_target_allocated = ctx.depth_view != nullptr;
    if (!result.depth_target_allocated) {
        result.error = "Renderer3D: failed to create depth target view";
        return std::nullopt;
    }

    return ctx;
}

bool finish_frame_readback(FrameContext& ctx, Scene3DRenderResult& result,
                           const wgpu::Buffer& readback_buffer, const PaddedRowLayout& layout) {
    std::string readback_error;
    auto pixels = read_padded_buffer(ctx.instance, readback_buffer, layout, std::chrono::seconds(2),
                                     readback_error);
    if (!pixels) {
        result.error = "Renderer3D: " + readback_error;
        return false;
    }
    result.rgba = std::move(*pixels);
    result.readback_completed = true;

    std::unordered_set<uint32_t> colors;
    uint32_t non_transparent = 0;
    for (size_t i = 0; i + 3 < result.rgba.size(); i += 4) {
        const uint32_t color = static_cast<uint32_t>(result.rgba[i]) |
                               (static_cast<uint32_t>(result.rgba[i + 1]) << 8u) |
                               (static_cast<uint32_t>(result.rgba[i + 2]) << 16u) |
                               (static_cast<uint32_t>(result.rgba[i + 3]) << 24u);
        colors.insert(color);
        if (result.rgba[i + 3] != 0) {
            ++non_transparent;
        }
    }
    result.distinct_color_count = static_cast<uint32_t>(colors.size());
    result.non_transparent_pixel_count = non_transparent;

    std::string png_error;
    result.png = encode_png(result.rgba, ctx.width, ctx.height, png_error);
    if (result.png.empty()) {
        result.error = png_error.empty() ? "Renderer3D: PNG encode failed" : png_error;
        return false;
    }
    return true;
}

} // namespace detail
} // namespace pulp::vellum_renderer3d
