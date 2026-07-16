// widget_bridge/gpu_api.cpp - native GPU JS registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "gpu_common.hpp"

#if __has_include(<pulp/render/gpu_surface.hpp>)
#include <pulp/render/gpu_surface.hpp>
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 1
#else
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 0
#endif

#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifdef PULP_HAS_SKIA
#include "webgpu/webgpu_cpp.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/graphite/Image.h"
#endif

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#endif

namespace pulp::view {

namespace {

#ifdef PULP_HAS_SKIA
wgpu::TextureFormat texture_format_from_string(const std::string& format) {
    if (format == "rgba16float") return wgpu::TextureFormat::RGBA16Float;
    if (format == "rgba8unorm") return wgpu::TextureFormat::RGBA8Unorm;
    if (format == "bgra8unorm-srgb") return wgpu::TextureFormat::BGRA8UnormSrgb;
    if (format == "rgba8unorm-srgb") return wgpu::TextureFormat::RGBA8UnormSrgb;
    return wgpu::TextureFormat::BGRA8Unorm;
}

wgpu::PrimitiveTopology primitive_topology_from_string(const std::string& topology) {
    if (topology == "point-list") return wgpu::PrimitiveTopology::PointList;
    if (topology == "line-list") return wgpu::PrimitiveTopology::LineList;
    if (topology == "line-strip") return wgpu::PrimitiveTopology::LineStrip;
    if (topology == "triangle-strip") return wgpu::PrimitiveTopology::TriangleStrip;
    return wgpu::PrimitiveTopology::TriangleList;
}

wgpu::VertexFormat vertex_format_from_string(const std::string& format) {
    if (format == "float32") return wgpu::VertexFormat::Float32;
    if (format == "float32x2") return wgpu::VertexFormat::Float32x2;
    if (format == "float32x3") return wgpu::VertexFormat::Float32x3;
    if (format == "float32x4") return wgpu::VertexFormat::Float32x4;
    if (format == "uint32") return wgpu::VertexFormat::Uint32;
    if (format == "uint32x2") return wgpu::VertexFormat::Uint32x2;
    if (format == "uint32x3") return wgpu::VertexFormat::Uint32x3;
    if (format == "uint32x4") return wgpu::VertexFormat::Uint32x4;
    if (format == "sint32") return wgpu::VertexFormat::Sint32;
    if (format == "sint32x2") return wgpu::VertexFormat::Sint32x2;
    if (format == "sint32x3") return wgpu::VertexFormat::Sint32x3;
    if (format == "sint32x4") return wgpu::VertexFormat::Sint32x4;
    return wgpu::VertexFormat::Float32x2;
}

wgpu::VertexStepMode vertex_step_mode_from_string(const std::string& step_mode) {
    if (step_mode == "instance") return wgpu::VertexStepMode::Instance;
    return wgpu::VertexStepMode::Vertex;
}

wgpu::IndexFormat index_format_from_string(const std::string& format) {
    if (format == "uint16") return wgpu::IndexFormat::Uint16;
    return wgpu::IndexFormat::Uint32;
}

wgpu::BufferBindingType buffer_binding_type_from_string(const std::string& type) {
    if (type == "storage") return wgpu::BufferBindingType::Storage;
    if (type == "read-only-storage" || type == "readonly-storage") return wgpu::BufferBindingType::ReadOnlyStorage;
    return wgpu::BufferBindingType::Uniform;
}

wgpu::BufferUsage buffer_usage_for_binding_type(wgpu::BufferBindingType type) {
    switch (type) {
        case wgpu::BufferBindingType::Storage:
        case wgpu::BufferBindingType::ReadOnlyStorage:
            return wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        case wgpu::BufferBindingType::BindingNotUsed:
            return wgpu::BufferUsage::CopyDst;
        case wgpu::BufferBindingType::Undefined:
        case wgpu::BufferBindingType::Uniform:
        default:
            return wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    }
}

wgpu::AddressMode address_mode_from_string(const std::string& mode) {
    if (mode == "repeat") return wgpu::AddressMode::Repeat;
    if (mode == "mirror-repeat") return wgpu::AddressMode::MirrorRepeat;
    return wgpu::AddressMode::ClampToEdge;
}

wgpu::FilterMode filter_mode_from_string(const std::string& mode) {
    if (mode == "linear") return wgpu::FilterMode::Linear;
    return wgpu::FilterMode::Nearest;
}

wgpu::MipmapFilterMode mipmap_filter_mode_from_string(const std::string& mode) {
    if (mode == "linear") return wgpu::MipmapFilterMode::Linear;
    return wgpu::MipmapFilterMode::Nearest;
}

wgpu::TextureViewDimension texture_view_dimension_from_string(const std::string& dimension) {
    if (dimension == "1d") return wgpu::TextureViewDimension::e1D;
    if (dimension == "2d-array") return wgpu::TextureViewDimension::e2DArray;
    if (dimension == "cube") return wgpu::TextureViewDimension::Cube;
    if (dimension == "cube-array") return wgpu::TextureViewDimension::CubeArray;
    if (dimension == "3d") return wgpu::TextureViewDimension::e3D;
    return wgpu::TextureViewDimension::e2D;
}

wgpu::TextureAspect texture_aspect_from_string(const std::string& aspect) {
    if (aspect == "stencil-only") return wgpu::TextureAspect::StencilOnly;
    if (aspect == "depth-only") return wgpu::TextureAspect::DepthOnly;
    if (aspect == "plane0-only") return wgpu::TextureAspect::Plane0Only;
    if (aspect == "plane1-only") return wgpu::TextureAspect::Plane1Only;
    if (aspect == "plane2-only") return wgpu::TextureAspect::Plane2Only;
    return wgpu::TextureAspect::All;
}

std::vector<uint8_t> json_bytes_to_vector(const choc::value::ValueView& value) {
    std::vector<uint8_t> bytes;
    if (!value.isArray()) return bytes;
    bytes.reserve(value.size());
    for (uint32_t i = 0; i < value.size(); ++i) {
        bytes.push_back(static_cast<uint8_t>(std::clamp(value[i].getWithDefault<int32_t>(0), 0, 255)));
    }
    return bytes;
}

std::vector<uint8_t> pad_webgpu_write_bytes(std::vector<uint8_t> bytes) {
    if (bytes.empty()) return bytes;
    auto remainder = bytes.size() % 4;
    if (remainder != 0) {
        bytes.resize(bytes.size() + (4 - remainder), 0);
    }
    return bytes;
}

uint32_t texture_bytes_per_pixel_from_format(const std::string& format) {
    if (format == "rgba16float") {
        return 8;
    }
    if (format == "rgba8unorm" || format == "bgra8unorm" ||
        format == "rgba8unorm-srgb" || format == "bgra8unorm-srgb") {
        return 4;
    }
    return 4;
}

wgpu::TextureUsage texture_usage_from_mask(uint32_t usage_mask) {
    if (usage_mask == 0) {
        return wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    }
    return static_cast<wgpu::TextureUsage>(usage_mask);
}
#endif

} // namespace

void BridgeRegistrars::register_gpu_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};
    // WebGPU: getGPUInfo() -> device capabilities
    register_bridge_function(api, "getGPUInfo", [&self](choc::javascript::ArgumentList) {
        auto gpu_info = detail::widget_bridge_gpu_info(self.gpu_surface_);
        auto info = choc::value::createObject("");
        info.addMember("backend", choc::value::createString(gpu_info.backend));
        info.addMember("backendType", choc::value::createString(gpu_info.backend_type));
        info.addMember("adapterName", choc::value::createString(gpu_info.name));
        info.addMember("available", choc::value::createBool(gpu_info.available));
        info.addMember("nativeBridge", choc::value::createBool(gpu_info.native_bridge));
        info.addMember("preferredCanvasFormat", choc::value::createString(gpu_info.preferred_canvas_format));
        #ifdef PULP_HAS_SKIA
        info.addMember("skia", choc::value::createBool(true));
        #else
        info.addMember("skia", choc::value::createBool(false));
        #endif
        return info;
    });

    auto gpu_info = detail::widget_bridge_gpu_info(self.gpu_surface_);
    HostObjectDescriptor gpu;
    gpu.class_name = "GPU";
    gpu.properties.push_back({"backend", choc::value::createString(gpu_info.backend)});
    gpu.properties.push_back({"backendType", choc::value::createString(gpu_info.backend_type)});
    gpu.properties.push_back({"available", choc::value::createBool(gpu_info.available)});
    gpu.properties.push_back({"nativeBridge", choc::value::createBool(gpu_info.native_bridge)});
    gpu.methods.push_back({"getPreferredCanvasFormat", [&self](const choc::value::Value*, size_t) {
        return choc::value::createString(detail::widget_bridge_gpu_info(self.gpu_surface_).preferred_canvas_format);
    }});
    register_bridge_host_object(api, "navigatorGPU", std::move(gpu));
    register_bridge_function(api, "__describeNativeAdapterImpl", [&self](choc::javascript::ArgumentList) {
        return detail::gpu_descriptor_to_value(detail::widget_bridge_gpu_info(self.gpu_surface_));
    });
    register_bridge_function(api, "__describeNativeDeviceImpl", [&self](choc::javascript::ArgumentList) {
        auto gpu_info = detail::widget_bridge_gpu_info(self.gpu_surface_);
        auto device = choc::value::createObject("");
        device.addMember("nativeBridge", choc::value::createBool(gpu_info.native_bridge));
        device.addMember("adapterInfo", detail::gpu_adapter_info_to_value(gpu_info));
        return device;
    });

    register_bridge_function(api, "__gpuCanvasConfigureImpl", [&self](choc::javascript::ArgumentList args) {
        auto gpu_info = detail::widget_bridge_gpu_info(self.gpu_surface_);
        auto canvas_id = args.get<std::string>(0, "");
        auto width = static_cast<uint32_t>(std::max(1, args.get<int32_t>(1, 1)));
        auto height = static_cast<uint32_t>(std::max(1, args.get<int32_t>(2, 1)));
        auto format = args.get<std::string>(3, gpu_info.preferred_canvas_format);
        auto usage = static_cast<uint32_t>(args.get<int32_t>(4, 0));
        auto alpha_mode = args.get<std::string>(5, "opaque");

        // Surface the GpuSurface's presentable/offscreen distinction to JS.
        // `presentable: true` is the program's contract that JS draws will hit
        // the visible swapchain rather than a silent offscreen texture.
        //
        // `has_surface()` is a member of render::GpuSurface; the full type
        // is only visible when pulp/render/gpu_surface.hpp is on the
        // include path. On no-GPU configures (e.g. iOS Simulator with
        // PULP_ENABLE_GPU=OFF) the render module isn't built, the header
        // isn't reachable, and GpuSurface stays forward-declared - so we
        // must gate the call under PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE and
        // report presentable=false when GPU is unavailable.
#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
        const bool presentable = (self.gpu_surface_ != nullptr) && self.gpu_surface_->has_surface();
#else
        const bool presentable = false;
#endif

        auto result = choc::value::createObject("");
        result.addMember("nativeBridge", choc::value::createBool(false));
        result.addMember("configured", choc::value::createBool(false));
        result.addMember("presentable", choc::value::createBool(presentable));
        result.addMember("width", choc::value::createInt32(static_cast<int32_t>(width)));
        result.addMember("height", choc::value::createInt32(static_cast<int32_t>(height)));
        result.addMember("format", choc::value::createString(format));
        result.addMember("usage", choc::value::createInt32(static_cast<int32_t>(usage)));
        result.addMember("alphaMode", choc::value::createString(alpha_mode));

        if (canvas_id.empty() || self.native_gpu_bridge_state_ == nullptr || self.gpu_surface_ == nullptr) {
            return result;
        }

#ifndef PULP_HAS_SKIA
        return result;
#else
        auto* device_ptr = static_cast<wgpu::Device*>(self.gpu_surface_->dawn_device_handle());
        if (device_ptr == nullptr) {
            return result;
        }

        auto& state = self.native_gpu_bridge_state_->canvases[canvas_id];
        state.width = width;
        state.height = height;
        state.format = format;
        state.usage = usage;
        state.alpha_mode = alpha_mode;
        state.configured = false;

        wgpu::TextureDescriptor texture_desc{};
        texture_desc.label = "Pulp Native GPUCanvasContext";
        texture_desc.dimension = wgpu::TextureDimension::e2D;
        texture_desc.size = { width, height, 1 };
        texture_desc.format = texture_format_from_string(format);
        texture_desc.mipLevelCount = 1;
        texture_desc.sampleCount = 1;
        auto requested_usage = usage == 0 ? static_cast<uint32_t>(wgpu::TextureUsage::RenderAttachment) : usage;
        auto texture_usage = static_cast<wgpu::TextureUsage>(requested_usage);
        if ((texture_usage & wgpu::TextureUsage::TextureBinding) == wgpu::TextureUsage::None) {
            texture_usage |= wgpu::TextureUsage::TextureBinding;
        }
        texture_desc.usage = texture_usage;
        state.usage = static_cast<uint32_t>(texture_desc.usage);
        state.texture = device_ptr->CreateTexture(&texture_desc);
        state.configured = (state.texture != nullptr);
        if (state.configured) {
            // Surface the presentable distinction in the runtime log so device
            // walks can grep the value without introspecting the JS-returned
            // object. The `presentable` value comes from
            // gpu_surface_->has_surface() above.
            runtime::log_info(
                "PULP_WEBGPU_BRIDGE: canvas.getContext('webgpu') ok (presentable={}, canvas={})",
                presentable ? "true" : "false", canvas_id);
            runtime::log_info(
                "PULP_WEBGPU_BRIDGE: context.configure ok (format={}, size={}x{})",
                format, width, height);
        }
        auto native_result = choc::value::createObject("");
        native_result.addMember("nativeBridge", choc::value::createBool(state.configured));
        native_result.addMember("configured", choc::value::createBool(state.configured));
        native_result.addMember("presentable", choc::value::createBool(presentable));
        native_result.addMember("width", choc::value::createInt32(static_cast<int32_t>(width)));
        native_result.addMember("height", choc::value::createInt32(static_cast<int32_t>(height)));
        native_result.addMember("format", choc::value::createString(format));
        native_result.addMember("usage", choc::value::createInt32(static_cast<int32_t>(state.usage)));
        native_result.addMember("alphaMode", choc::value::createString(alpha_mode));
        return native_result;
#endif
    });

    register_bridge_function(api, "__gpuCanvasDescribeCurrentTextureImpl", [&self](choc::javascript::ArgumentList args) {
        // Surface the presentable flag here too so JS can verify per-frame that
        // the texture it is about to draw into is the visible swapchain. Same
        // PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE gate as the configure path above:
        // no-GPU configures keep presentable=false because there is no
        // swapchain to address.
#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
        const bool presentable = (self.gpu_surface_ != nullptr) && self.gpu_surface_->has_surface();
#else
        const bool presentable = false;
#endif

        auto descriptor = choc::value::createObject("");
        auto canvas_id = args.get<std::string>(0, "");
        descriptor.addMember("nativeBridge", choc::value::createBool(false));
        descriptor.addMember("presentable", choc::value::createBool(presentable));
        descriptor.addMember("width", choc::value::createInt32(1));
        descriptor.addMember("height", choc::value::createInt32(1));
        descriptor.addMember("format", choc::value::createString("bgra8unorm"));
        descriptor.addMember("usage", choc::value::createInt32(0));
        descriptor.addMember("label", choc::value::createString("pulp-native-gpu-texture"));

        if (canvas_id.empty() || self.native_gpu_bridge_state_ == nullptr) {
            return descriptor;
        }

        auto it = self.native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == self.native_gpu_bridge_state_->canvases.end() || !it->second.configured) {
            return descriptor;
        }

        auto native_descriptor = choc::value::createObject("");
        native_descriptor.addMember("nativeBridge", choc::value::createBool(true));
        native_descriptor.addMember("presentable", choc::value::createBool(presentable));
        native_descriptor.addMember("width", choc::value::createInt32(static_cast<int32_t>(it->second.width)));
        native_descriptor.addMember("height", choc::value::createInt32(static_cast<int32_t>(it->second.height)));
        native_descriptor.addMember("format", choc::value::createString(it->second.format));
        native_descriptor.addMember("usage", choc::value::createInt32(static_cast<int32_t>(it->second.usage)));
        native_descriptor.addMember("label", choc::value::createString(canvas_id + "-native-texture"));
        return native_descriptor;
    });

    register_bridge_function(api, "__gpuCreateTextureImpl", [&self](choc::javascript::ArgumentList args) {
        auto payload_json = args.get<std::string>(0, "");
        if (payload_json.empty() || self.native_gpu_bridge_state_ == nullptr || self.gpu_surface_ == nullptr) {
            return choc::value::createString("");
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createString("");
#else
        choc::value::Value payload;
        try {
            payload = choc::json::parse(payload_json);
        } catch (...) {
            return choc::value::createString("");
        }

        auto* device_ptr = static_cast<wgpu::Device*>(self.gpu_surface_->dawn_device_handle());
        if (device_ptr == nullptr || !(*device_ptr)) {
            return choc::value::createString("");
        }

        auto size_view = payload.hasObjectMember("size") ? payload["size"] : choc::value::Value();
        auto width = static_cast<uint32_t>(std::max(1, size_view.hasObjectMember("width")
            ? size_view["width"].getWithDefault<int32_t>(1) : 1));
        auto height = static_cast<uint32_t>(std::max(1, size_view.hasObjectMember("height")
            ? size_view["height"].getWithDefault<int32_t>(1) : 1));
        auto depth_or_array_layers = static_cast<uint32_t>(std::max(1, size_view.hasObjectMember("depthOrArrayLayers")
            ? size_view["depthOrArrayLayers"].getWithDefault<int32_t>(1) : 1));
        auto format = payload.hasObjectMember("format")
            ? payload["format"].getWithDefault<std::string>("bgra8unorm")
            : "bgra8unorm";
        auto usage = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("usage")
            ? payload["usage"].getWithDefault<int32_t>(0) : 0));
        auto mip_level_count = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("mipLevelCount")
            ? payload["mipLevelCount"].getWithDefault<int32_t>(1) : 1));
        auto sample_count = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("sampleCount")
            ? payload["sampleCount"].getWithDefault<int32_t>(1) : 1));

        wgpu::TextureDescriptor texture_desc{};
        texture_desc.label = "Pulp Native GPUTexture";
        texture_desc.dimension = wgpu::TextureDimension::e2D;
        texture_desc.size = { width, height, depth_or_array_layers };
        texture_desc.format = texture_format_from_string(format);
        texture_desc.mipLevelCount = mip_level_count;
        texture_desc.sampleCount = sample_count;
        texture_desc.usage = texture_usage_from_mask(usage);
        if ((texture_desc.usage & wgpu::TextureUsage::TextureBinding) == wgpu::TextureUsage::None) {
            texture_desc.usage |= wgpu::TextureUsage::TextureBinding;
        }
        if ((texture_desc.usage & wgpu::TextureUsage::CopyDst) == wgpu::TextureUsage::None) {
            texture_desc.usage |= wgpu::TextureUsage::CopyDst;
        }
        if ((texture_desc.usage & wgpu::TextureUsage::RenderAttachment) == wgpu::TextureUsage::None) {
            texture_desc.usage |= wgpu::TextureUsage::RenderAttachment;
        }

        auto texture = device_ptr->CreateTexture(&texture_desc);
        if (!texture) {
            return choc::value::createString("");
        }

        auto texture_id = std::string("native-texture-") + std::to_string(self.native_gpu_bridge_state_->next_texture_id++);
        auto& state = self.native_gpu_bridge_state_->textures[texture_id];
        state.width = width;
        state.height = height;
        state.depth_or_array_layers = depth_or_array_layers;
        state.format = format;
        state.usage = static_cast<uint32_t>(texture_desc.usage);
        state.mip_level_count = mip_level_count;
        state.sample_count = sample_count;
        state.texture = texture;
        state.configured = true;
        return choc::value::createString(texture_id);
#endif
    });

    register_bridge_function(api, "__gpuQueueWriteTextureImpl", [&self](choc::javascript::ArgumentList args) {
        auto payload_json = args.get<std::string>(0, "");
        if (payload_json.empty() || self.native_gpu_bridge_state_ == nullptr || self.gpu_surface_ == nullptr) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createBool(false);
#else
        choc::value::Value payload;
        try {
            payload = choc::json::parse(payload_json);
        } catch (...) {
            return choc::value::createBool(false);
        }

        auto texture_id = payload.hasObjectMember("textureId")
            ? payload["textureId"].getWithDefault<std::string>("")
            : "";
        auto texture_it = self.native_gpu_bridge_state_->textures.find(texture_id);
        if (texture_id.empty() || texture_it == self.native_gpu_bridge_state_->textures.end() ||
            !texture_it->second.configured || !texture_it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* queue_ptr = static_cast<wgpu::Queue*>(self.gpu_surface_->dawn_queue_handle());
        if (queue_ptr == nullptr || !(*queue_ptr)) {
            return choc::value::createBool(false);
        }

        auto texture_bytes = payload.hasObjectMember("data")
            ? json_bytes_to_vector(payload["data"])
            : std::vector<uint8_t>{};
        if (texture_bytes.empty()) {
            return choc::value::createBool(false);
        }

        auto width = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("width")
            ? payload["width"].getWithDefault<int32_t>(static_cast<int32_t>(texture_it->second.width))
            : static_cast<int32_t>(texture_it->second.width)));
        auto height = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("height")
            ? payload["height"].getWithDefault<int32_t>(static_cast<int32_t>(texture_it->second.height))
            : static_cast<int32_t>(texture_it->second.height)));
        auto depth_or_array_layers = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("depthOrArrayLayers")
            ? payload["depthOrArrayLayers"].getWithDefault<int32_t>(1)
            : 1));
        auto bytes_per_row = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("bytesPerRow")
            ? payload["bytesPerRow"].getWithDefault<int32_t>(0)
            : 0));
        auto rows_per_image = static_cast<uint32_t>(std::max<int32_t>(1, payload.hasObjectMember("rowsPerImage")
            ? payload["rowsPerImage"].getWithDefault<int32_t>(static_cast<int32_t>(height))
            : static_cast<int32_t>(height)));
        if (bytes_per_row == 0) {
            bytes_per_row = width * texture_bytes_per_pixel_from_format(texture_it->second.format);
        }

        auto mip_level = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("mipLevel")
            ? payload["mipLevel"].getWithDefault<int32_t>(0)
            : 0));

        uint32_t origin_x = 0;
        uint32_t origin_y = 0;
        uint32_t origin_z = 0;
        if (payload.hasObjectMember("origin") && payload["origin"].isObject()) {
            auto origin = payload["origin"];
            origin_x = static_cast<uint32_t>(std::max(0, origin.hasObjectMember("x") ? origin["x"].getWithDefault<int32_t>(0) : 0));
            origin_y = static_cast<uint32_t>(std::max(0, origin.hasObjectMember("y") ? origin["y"].getWithDefault<int32_t>(0) : 0));
            origin_z = static_cast<uint32_t>(std::max(0, origin.hasObjectMember("z") ? origin["z"].getWithDefault<int32_t>(0) : 0));
        }

        wgpu::TexelCopyTextureInfo destination{};
        destination.texture = texture_it->second.texture;
        destination.mipLevel = mip_level;
        destination.origin = { origin_x, origin_y, origin_z };
        destination.aspect = wgpu::TextureAspect::All;

        wgpu::TexelCopyBufferLayout data_layout{};
        data_layout.offset = 0;
        data_layout.bytesPerRow = bytes_per_row;
        data_layout.rowsPerImage = rows_per_image;

        wgpu::Extent3D write_size{};
        write_size.width = width;
        write_size.height = height;
        write_size.depthOrArrayLayers = depth_or_array_layers;
        queue_ptr->WriteTexture(&destination, texture_bytes.data(), texture_bytes.size(), &data_layout, &write_size);
        return choc::value::createBool(true);
#endif
    });

    register_bridge_function(api, "__decodeImageDataImpl", [](choc::javascript::ArgumentList args) {
        auto result = choc::value::createObject("");
        result.addMember("ok", choc::value::createBool(false));

#ifdef PULP_HAS_SKIA
        auto payload_json = args.get<std::string>(0, "");
        if (payload_json.empty()) return result;

        choc::value::Value payload;
        try {
            payload = choc::json::parse(payload_json);
        } catch (...) {
            return result;
        }

        if (!payload.hasObjectMember("data")) return result;
        auto encoded_bytes = json_bytes_to_vector(payload["data"]);
        if (encoded_bytes.empty()) return result;

        auto sk_data = SkData::MakeWithoutCopy(encoded_bytes.data(), encoded_bytes.size());
        auto image = SkImages::DeferredFromEncodedData(sk_data);
        if (!image) return result;

        auto width = image->width();
        auto height = image->height();
        if (width <= 0 || height <= 0) return result;

        auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        if (!image->readPixels(info, pixels.data(), static_cast<size_t>(width) * 4, 0, 0)) {
            return result;
        }

        auto pixel_array = choc::value::createEmptyArray();
        for (auto byte : pixels) {
            pixel_array.addArrayElement(choc::value::createInt32(static_cast<int32_t>(byte)));
        }

        result.setMember("ok", choc::value::createBool(true));
        result.addMember("width", choc::value::createInt32(width));
        result.addMember("height", choc::value::createInt32(height));
        result.addMember("pixels", pixel_array);
#endif
        return result;
    });

    register_bridge_function(api, "__gpuDestroyTextureImpl", [&self](choc::javascript::ArgumentList args) {
        auto texture_id = args.get<std::string>(0, "");
        if (texture_id.empty() || self.native_gpu_bridge_state_ == nullptr) {
            return choc::value::createBool(false);
        }

        return choc::value::createBool(self.native_gpu_bridge_state_->textures.erase(texture_id) > 0);
    });

    register_bridge_function(api, "__gpuQueueSubmitImpl", [&self](choc::javascript::ArgumentList args) {
        auto canvas_id = args.get<std::string>(0, "");
        auto r = static_cast<float>(args.get<double>(1, 0.0));
        auto g = static_cast<float>(args.get<double>(2, 0.0));
        auto b = static_cast<float>(args.get<double>(3, 0.0));
        auto a = static_cast<float>(args.get<double>(4, 1.0));

        if (canvas_id.empty() || self.native_gpu_bridge_state_ == nullptr || self.gpu_surface_ == nullptr) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        (void)r;
        (void)g;
        (void)b;
        (void)a;
        return choc::value::createBool(false);
#else
        auto it = self.native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == self.native_gpu_bridge_state_->canvases.end() || !it->second.configured || !it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* device_ptr = static_cast<wgpu::Device*>(self.gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(self.gpu_surface_->dawn_queue_handle());
        if (device_ptr == nullptr || queue_ptr == nullptr || !(*device_ptr) || !(*queue_ptr)) {
            return choc::value::createBool(false);
        }

        auto texture_view = it->second.texture.CreateView();
        if (!texture_view) {
            return choc::value::createBool(false);
        }

        wgpu::RenderPassColorAttachment color_attachment{};
        color_attachment.view = texture_view;
        color_attachment.loadOp = wgpu::LoadOp::Clear;
        color_attachment.storeOp = wgpu::StoreOp::Store;
        color_attachment.clearValue = {r, g, b, a};

        wgpu::RenderPassDescriptor pass_desc{};
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color_attachment;

        wgpu::CommandEncoderDescriptor encoder_desc{};
        auto encoder = device_ptr->CreateCommandEncoder(&encoder_desc);
        if (!encoder) {
            return choc::value::createBool(false);
        }

        auto pass = encoder.BeginRenderPass(&pass_desc);
        pass.End();

        auto command_buffer = encoder.Finish();
        queue_ptr->Submit(1, &command_buffer);

        // Surface queue.submit success in the runtime log so device walks can
        // grep `PULP_WEBGPU_BRIDGE: queue.submit ok` without instrumenting JS.
        // `commands=1` is accurate for this single-command-buffer code path;
        // the buffered shim (`__gpuQueueDrawBufferedImpl` etc.) handles
        // multi-command submissions and would emit a different count.
        runtime::log_info(
            "PULP_WEBGPU_BRIDGE: queue.submit ok (canvas={}, commands=1)",
            canvas_id);
        return choc::value::createBool(true);
#endif
    });

    register_bridge_function(api, "__gpuQueueDrawImpl", [&self](choc::javascript::ArgumentList args) {
        auto canvas_id = args.get<std::string>(0, "");
        auto vertex_code = args.get<std::string>(1, "");
        auto vertex_entry = args.get<std::string>(2, "main");
        auto fragment_code = args.get<std::string>(3, "");
        auto fragment_entry = args.get<std::string>(4, "main");
        auto format = args.get<std::string>(5, "bgra8unorm");
        auto topology = args.get<std::string>(6, "triangle-list");
        auto vertex_count = static_cast<uint32_t>(std::max(0, args.get<int32_t>(7, 0)));
        auto instance_count = static_cast<uint32_t>(std::max(1, args.get<int32_t>(8, 1)));
        auto first_vertex = static_cast<uint32_t>(std::max(0, args.get<int32_t>(9, 0)));
        auto first_instance = static_cast<uint32_t>(std::max(0, args.get<int32_t>(10, 0)));
        auto bind_groups_json = args.get<std::string>(11, "");

        if (canvas_id.empty() || self.native_gpu_bridge_state_ == nullptr || self.gpu_surface_ == nullptr ||
            vertex_code.empty() || fragment_code.empty() || vertex_count == 0) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        (void)instance_count;
        (void)first_vertex;
        (void)first_instance;
        return choc::value::createBool(false);
#else
        auto it = self.native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == self.native_gpu_bridge_state_->canvases.end() || !it->second.configured || !it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* device_ptr = static_cast<wgpu::Device*>(self.gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(self.gpu_surface_->dawn_queue_handle());
        if (device_ptr == nullptr || queue_ptr == nullptr || !(*device_ptr) || !(*queue_ptr)) {
            return choc::value::createBool(false);
        }

        wgpu::ShaderSourceWGSL vertex_wgsl{};
        vertex_wgsl.code = vertex_code.c_str();
        wgpu::ShaderModuleDescriptor vertex_module_desc{};
        vertex_module_desc.nextInChain = &vertex_wgsl;
        auto vertex_module = device_ptr->CreateShaderModule(&vertex_module_desc);
        if (!vertex_module) {
            return choc::value::createBool(false);
        }

        wgpu::ShaderSourceWGSL fragment_wgsl{};
        fragment_wgsl.code = fragment_code.c_str();
        wgpu::ShaderModuleDescriptor fragment_module_desc{};
        fragment_module_desc.nextInChain = &fragment_wgsl;
        auto fragment_module = device_ptr->CreateShaderModule(&fragment_module_desc);
        if (!fragment_module) {
            return choc::value::createBool(false);
        }

        wgpu::ColorTargetState color_target{};
        color_target.format = texture_format_from_string(format);
        color_target.writeMask = wgpu::ColorWriteMask::All;

        wgpu::FragmentState fragment_state{};
        fragment_state.module = fragment_module;
        fragment_state.entryPoint = fragment_entry.c_str();
        fragment_state.targetCount = 1;
        fragment_state.targets = &color_target;

        wgpu::RenderPipelineDescriptor pipeline_desc{};
        pipeline_desc.layout = nullptr;
        pipeline_desc.vertex.module = vertex_module;
        pipeline_desc.vertex.entryPoint = vertex_entry.c_str();
        pipeline_desc.fragment = &fragment_state;
        pipeline_desc.primitive.topology = primitive_topology_from_string(topology);
        pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;
        pipeline_desc.primitive.cullMode = wgpu::CullMode::None;
        pipeline_desc.multisample.count = 1;
        pipeline_desc.multisample.mask = ~0u;
        pipeline_desc.multisample.alphaToCoverageEnabled = false;

        auto pipeline = device_ptr->CreateRenderPipeline(&pipeline_desc);
        if (!pipeline) {
            return choc::value::createBool(false);
        }

        std::vector<uint32_t> bind_group_indices;
        std::vector<wgpu::Buffer> bind_group_buffers;
        std::vector<wgpu::Sampler> bind_group_samplers;
        std::vector<wgpu::Texture> bind_group_textures;
        std::vector<wgpu::TextureView> bind_group_texture_views;
        std::vector<wgpu::BindGroup> bind_groups;
        if (!bind_groups_json.empty()) {
            choc::value::Value bind_groups_payload;
            try {
                bind_groups_payload = choc::json::parse(bind_groups_json);
            } catch (...) {
                return choc::value::createBool(false);
            }

            if (!bind_groups_payload.isArray()) {
                return choc::value::createBool(false);
            }

            bind_group_indices.reserve(bind_groups_payload.size());
            bind_groups.reserve(bind_groups_payload.size());

            for (uint32_t i = 0; i < bind_groups_payload.size(); ++i) {
                auto bind_group_view = bind_groups_payload[i];
                if (!bind_group_view.isObject() || !bind_group_view.hasObjectMember("entries") || !bind_group_view["entries"].isArray()) {
                    return choc::value::createBool(false);
                }

                auto group_index = static_cast<uint32_t>(std::max(0, bind_group_view.hasObjectMember("index")
                    ? bind_group_view["index"].getWithDefault<int32_t>(0)
                    : 0));
                auto layout = pipeline.GetBindGroupLayout(group_index);
                if (!layout) {
                    return choc::value::createBool(false);
                }

                auto entries_view = bind_group_view["entries"];
                std::vector<wgpu::BindGroupEntry> bind_group_entries;
                bind_group_entries.reserve(entries_view.size());

                for (uint32_t j = 0; j < entries_view.size(); ++j) {
                    auto entry_view = entries_view[j];
                    if (!entry_view.isObject()) {
                        return choc::value::createBool(false);
                    }

                    auto resource_type = entry_view.hasObjectMember("resourceType")
                        ? entry_view["resourceType"].getWithDefault<std::string>("buffer")
                        : "buffer";
                    auto binding = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("binding")
                        ? entry_view["binding"].getWithDefault<int32_t>(0)
                        : 0));
                    wgpu::BindGroupEntry bind_group_entry{};
                    bind_group_entry.binding = binding;

                    if (resource_type == "buffer") {
                        auto buffer_type = buffer_binding_type_from_string(entry_view.hasObjectMember("bufferType")
                            ? entry_view["bufferType"].getWithDefault<std::string>("uniform")
                            : "uniform");
                        auto bytes = entry_view.hasObjectMember("data")
                            ? json_bytes_to_vector(entry_view["data"])
                            : std::vector<uint8_t>{};
                        auto upload_data = pad_webgpu_write_bytes(std::move(bytes));
                        if (upload_data.empty()) {
                            return choc::value::createBool(false);
                        }

                        auto binding_size = static_cast<uint64_t>(std::max<int64_t>(0, entry_view.hasObjectMember("size")
                            ? entry_view["size"].getWithDefault<int64_t>(static_cast<int64_t>(upload_data.size()))
                            : static_cast<int64_t>(upload_data.size())));
                        if (binding_size == 0) {
                            binding_size = upload_data.size();
                        }

                        wgpu::BufferDescriptor buffer_desc{};
                        buffer_desc.usage = buffer_usage_for_binding_type(buffer_type);
                        buffer_desc.size = upload_data.size();
                        auto gpu_buffer = device_ptr->CreateBuffer(&buffer_desc);
                        if (!gpu_buffer) {
                            return choc::value::createBool(false);
                        }
#ifdef PULP_BENCHMARK
                        {
                            const double t0 = render::bench::now_us();
                            queue_ptr->WriteBuffer(gpu_buffer, 0, upload_data.data(), upload_data.size());
                            if (self.bench_counters_) {
                                self.bench_counters_->gpu_upload_total_us.fetch_add(
                                    render::bench::now_us() - t0,
                                    std::memory_order_relaxed);
                                self.bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                                    static_cast<double>(upload_data.size()),
                                    std::memory_order_relaxed);
                                self.bench_counters_->gpu_buffer_upload_count.fetch_add(
                                    1.0, std::memory_order_relaxed);
                                self.bench_counters_->observe_resident_peak(
                                    static_cast<double>(upload_data.size()));
                            }
                        }
#else
                        queue_ptr->WriteBuffer(gpu_buffer, 0, upload_data.data(), upload_data.size());
#endif
                        bind_group_buffers.push_back(gpu_buffer);

                        bind_group_entry.buffer = gpu_buffer;
                        bind_group_entry.offset = 0;
                        bind_group_entry.size = binding_size;
                        bind_group_entries.push_back(bind_group_entry);
                        continue;
                    }

                    if (resource_type == "sampler") {
                        wgpu::SamplerDescriptor sampler_desc{};
                        sampler_desc.addressModeU = address_mode_from_string(entry_view.hasObjectMember("addressModeU")
                            ? entry_view["addressModeU"].getWithDefault<std::string>("clamp-to-edge")
                            : "clamp-to-edge");
                        sampler_desc.addressModeV = address_mode_from_string(entry_view.hasObjectMember("addressModeV")
                            ? entry_view["addressModeV"].getWithDefault<std::string>("clamp-to-edge")
                            : "clamp-to-edge");
                        sampler_desc.addressModeW = address_mode_from_string(entry_view.hasObjectMember("addressModeW")
                            ? entry_view["addressModeW"].getWithDefault<std::string>("clamp-to-edge")
                            : "clamp-to-edge");
                        sampler_desc.magFilter = filter_mode_from_string(entry_view.hasObjectMember("magFilter")
                            ? entry_view["magFilter"].getWithDefault<std::string>("nearest")
                            : "nearest");
                        sampler_desc.minFilter = filter_mode_from_string(entry_view.hasObjectMember("minFilter")
                            ? entry_view["minFilter"].getWithDefault<std::string>("nearest")
                            : "nearest");
                        sampler_desc.mipmapFilter = mipmap_filter_mode_from_string(entry_view.hasObjectMember("mipmapFilter")
                            ? entry_view["mipmapFilter"].getWithDefault<std::string>("nearest")
                            : "nearest");
                        auto sampler = device_ptr->CreateSampler(&sampler_desc);
                        if (!sampler) {
                            return choc::value::createBool(false);
                        }
                        bind_group_samplers.push_back(sampler);
                        bind_group_entry.sampler = sampler;
                        bind_group_entries.push_back(bind_group_entry);
                        continue;
                    }

                    if (resource_type == "textureView") {
                        auto source_canvas_id = entry_view.hasObjectMember("sourceCanvasId")
                            ? entry_view["sourceCanvasId"].getWithDefault<std::string>("")
                            : "";
                        wgpu::TextureView texture_view;

                        auto default_view_format = source_canvas_id.empty() ? format : "bgra8unorm";
                        auto view_format = entry_view.hasObjectMember("format")
                            ? entry_view["format"].getWithDefault<std::string>(default_view_format)
                            : default_view_format;
                        auto view_dimension = entry_view.hasObjectMember("dimension")
                            ? entry_view["dimension"].getWithDefault<std::string>("2d")
                            : "2d";
                        auto view_aspect = entry_view.hasObjectMember("aspect")
                            ? entry_view["aspect"].getWithDefault<std::string>("all")
                            : "all";
                        auto base_mip_level = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("baseMipLevel")
                            ? entry_view["baseMipLevel"].getWithDefault<int32_t>(0)
                            : 0));
                        auto mip_level_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("mipLevelCount")
                            ? entry_view["mipLevelCount"].getWithDefault<int32_t>(1)
                            : 1));
                        auto base_array_layer = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("baseArrayLayer")
                            ? entry_view["baseArrayLayer"].getWithDefault<int32_t>(0)
                            : 0));
                        auto array_layer_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("arrayLayerCount")
                            ? entry_view["arrayLayerCount"].getWithDefault<int32_t>(1)
                            : 1));

                        if (!source_canvas_id.empty()) {
                            auto source_it = self.native_gpu_bridge_state_->canvases.find(source_canvas_id);
                            if (source_it == self.native_gpu_bridge_state_->canvases.end() ||
                                !source_it->second.configured || !source_it->second.texture) {
                                return choc::value::createBool(false);
                            }

                            const bool use_default_view =
                                view_format == source_it->second.format &&
                                view_dimension == "2d" &&
                                view_aspect == "all" &&
                                base_mip_level == 0 &&
                                mip_level_count == 1 &&
                                base_array_layer == 0 &&
                                array_layer_count == 1;

                            texture_view = use_default_view
                                ? source_it->second.texture.CreateView()
                                : [&]() {
                                    wgpu::TextureViewDescriptor texture_view_desc{};
                                    texture_view_desc.format = texture_format_from_string(view_format);
                                    texture_view_desc.dimension = texture_view_dimension_from_string(view_dimension);
                                    texture_view_desc.aspect = texture_aspect_from_string(view_aspect);
                                    texture_view_desc.baseMipLevel = base_mip_level;
                                    texture_view_desc.mipLevelCount = mip_level_count;
                                    texture_view_desc.baseArrayLayer = base_array_layer;
                                    texture_view_desc.arrayLayerCount = array_layer_count;
                                    return source_it->second.texture.CreateView(&texture_view_desc);
                                }();
                        } else {
                            auto texture_width = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("width")
                                ? entry_view["width"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_height = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("height")
                                ? entry_view["height"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_depth = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("depthOrArrayLayers")
                                ? entry_view["depthOrArrayLayers"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_usage_mask = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("usage")
                                ? entry_view["usage"].getWithDefault<int32_t>(0)
                                : 0));
                            auto texture_sample_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("sampleCount")
                                ? entry_view["sampleCount"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_mip_level_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("textureMipLevelCount")
                                ? entry_view["textureMipLevelCount"].getWithDefault<int32_t>(1)
                                : 1));
                            auto bytes_per_row = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("bytesPerRow")
                                ? entry_view["bytesPerRow"].getWithDefault<int32_t>(0)
                                : 0));
                            auto rows_per_image = static_cast<uint32_t>(std::max<int32_t>(1, entry_view.hasObjectMember("rowsPerImage")
                                ? entry_view["rowsPerImage"].getWithDefault<int32_t>(static_cast<int32_t>(texture_height))
                                : static_cast<int32_t>(texture_height)));
                            auto texture_bytes = entry_view.hasObjectMember("data")
                                ? json_bytes_to_vector(entry_view["data"])
                                : std::vector<uint8_t>{};
                            if (texture_bytes.empty()) {
                                return choc::value::createBool(false);
                            }

                            auto required_bytes_per_row = texture_width * texture_bytes_per_pixel_from_format(view_format);
                            if (bytes_per_row == 0) {
                                bytes_per_row = required_bytes_per_row;
                            }

                            wgpu::TextureDescriptor texture_desc{};
                            texture_desc.dimension = wgpu::TextureDimension::e2D;
                            texture_desc.size.width = texture_width;
                            texture_desc.size.height = texture_height;
                            texture_desc.size.depthOrArrayLayers = texture_depth;
                            texture_desc.format = texture_format_from_string(view_format);
                            texture_desc.usage = texture_usage_from_mask(texture_usage_mask);
                            texture_desc.mipLevelCount = texture_mip_level_count;
                            texture_desc.sampleCount = texture_sample_count;
                            if ((texture_desc.usage & wgpu::TextureUsage::TextureBinding) == wgpu::TextureUsage::None) {
                                texture_desc.usage |= wgpu::TextureUsage::TextureBinding;
                            }
                            if ((texture_desc.usage & wgpu::TextureUsage::CopyDst) == wgpu::TextureUsage::None) {
                                texture_desc.usage |= wgpu::TextureUsage::CopyDst;
                            }

                            auto uploaded_texture = device_ptr->CreateTexture(&texture_desc);
                            if (!uploaded_texture) {
                                return choc::value::createBool(false);
                            }

                            wgpu::TexelCopyTextureInfo destination{};
                            destination.texture = uploaded_texture;
                            destination.aspect = wgpu::TextureAspect::All;
                            wgpu::TexelCopyBufferLayout data_layout{};
                            data_layout.offset = 0;
                            data_layout.bytesPerRow = bytes_per_row;
                            data_layout.rowsPerImage = rows_per_image;
                            wgpu::Extent3D write_size{};
                            write_size.width = texture_width;
                            write_size.height = texture_height;
                            write_size.depthOrArrayLayers = texture_depth;
                            queue_ptr->WriteTexture(&destination, texture_bytes.data(), texture_bytes.size(), &data_layout, &write_size);

                            wgpu::TextureViewDescriptor texture_view_desc{};
                            texture_view_desc.format = texture_format_from_string(view_format);
                            texture_view_desc.dimension = texture_view_dimension_from_string(view_dimension);
                            texture_view_desc.aspect = texture_aspect_from_string(view_aspect);
                            texture_view_desc.baseMipLevel = base_mip_level;
                            texture_view_desc.mipLevelCount = mip_level_count;
                            texture_view_desc.baseArrayLayer = base_array_layer;
                            texture_view_desc.arrayLayerCount = array_layer_count;
                            texture_view = uploaded_texture.CreateView(&texture_view_desc);
                            bind_group_textures.push_back(uploaded_texture);
                        }

                        if (!texture_view) {
                            return choc::value::createBool(false);
                        }
                        bind_group_texture_views.push_back(texture_view);
                        bind_group_entry.textureView = texture_view;
                        bind_group_entries.push_back(bind_group_entry);
                        continue;
                    }

                    return choc::value::createBool(false);
                }

                wgpu::BindGroupDescriptor bind_group_desc{};
                bind_group_desc.layout = layout;
                bind_group_desc.entryCount = bind_group_entries.size();
                bind_group_desc.entries = bind_group_entries.data();
                auto bind_group = device_ptr->CreateBindGroup(&bind_group_desc);
                if (!bind_group) {
                    return choc::value::createBool(false);
                }

                bind_group_indices.push_back(group_index);
                bind_groups.push_back(bind_group);
            }
        }

        auto texture_view = it->second.texture.CreateView();
        if (!texture_view) {
            return choc::value::createBool(false);
        }

        wgpu::RenderPassColorAttachment color_attachment{};
        color_attachment.view = texture_view;
        color_attachment.loadOp = wgpu::LoadOp::Load;
        color_attachment.storeOp = wgpu::StoreOp::Store;

        wgpu::RenderPassDescriptor pass_desc{};
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color_attachment;

        wgpu::CommandEncoderDescriptor encoder_desc{};
        auto encoder = device_ptr->CreateCommandEncoder(&encoder_desc);
        if (!encoder) {
            return choc::value::createBool(false);
        }

        auto pass = encoder.BeginRenderPass(&pass_desc);
        pass.SetPipeline(pipeline);
        for (size_t i = 0; i < bind_groups.size(); ++i) {
            pass.SetBindGroup(bind_group_indices[i], bind_groups[i], 0, nullptr);
        }
        if (first_instance > 0 && device_ptr->HasFeature(wgpu::FeatureName::IndirectFirstInstance)) {
            runtime::log_info(
                "PULP_WEBGPU_BRIDGE: DrawIndirect/immediate firstInstance={} vertexCount={} instanceCount={}",
                first_instance, vertex_count, instance_count);
            uint32_t indirect_args[4] = { vertex_count, instance_count, first_vertex, first_instance };
            wgpu::BufferDescriptor ibd{};
            ibd.size = sizeof(indirect_args);
            ibd.usage = wgpu::BufferUsage::Indirect | wgpu::BufferUsage::CopyDst;
            auto ibuf = device_ptr->CreateBuffer(&ibd);
            queue_ptr->WriteBuffer(ibuf, 0, indirect_args, sizeof(indirect_args));
            pass.DrawIndirect(ibuf, 0);
        } else {
            pass.Draw(vertex_count, instance_count, first_vertex, first_instance);
        }
        pass.End();

        auto command_buffer = encoder.Finish();
        queue_ptr->Submit(1, &command_buffer);
        return choc::value::createBool(true);
#endif
    });

    register_bridge_function(api, "__gpuQueueDrawBufferedImpl", [&self](choc::javascript::ArgumentList args) {
        if (args.numArgs < 1 || !args[0] || self.native_gpu_bridge_state_ == nullptr || self.gpu_surface_ == nullptr) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createBool(false);
#else
        auto& payload = *args[0];
        if (!payload.isObject()) {
            return choc::value::createBool(false);
        }

        auto canvas_id = payload.hasObjectMember("canvasId") ? payload["canvasId"].getWithDefault<std::string>("") : "";
        auto target_texture_id = payload.hasObjectMember("targetTextureId") ? payload["targetTextureId"].getWithDefault<std::string>("") : "";
        auto vertex_code = payload.hasObjectMember("vertexCode") ? payload["vertexCode"].getWithDefault<std::string>("") : "";
        auto vertex_entry = payload.hasObjectMember("vertexEntryPoint") ? payload["vertexEntryPoint"].getWithDefault<std::string>("main") : "main";
        auto fragment_code = payload.hasObjectMember("fragmentCode") ? payload["fragmentCode"].getWithDefault<std::string>("") : "";
        auto fragment_entry = payload.hasObjectMember("fragmentEntryPoint") ? payload["fragmentEntryPoint"].getWithDefault<std::string>("main") : "main";
        auto format = payload.hasObjectMember("format") ? payload["format"].getWithDefault<std::string>("bgra8unorm") : "bgra8unorm";
        auto topology = payload.hasObjectMember("topology") ? payload["topology"].getWithDefault<std::string>("triangle-list") : "triangle-list";
        auto draw_type = payload.hasObjectMember("drawType") ? payload["drawType"].getWithDefault<std::string>("draw") : "draw";
        auto load_op = payload.hasObjectMember("loadOp") ? payload["loadOp"].getWithDefault<std::string>("load") : "load";
        auto store_op = payload.hasObjectMember("storeOp") ? payload["storeOp"].getWithDefault<std::string>("store") : "store";

        if ((canvas_id.empty() && target_texture_id.empty()) || vertex_code.empty() || fragment_code.empty() ||
            !payload.hasObjectMember("vertexBuffers") || !payload["vertexBuffers"].isArray() ||
            payload["vertexBuffers"].size() == 0) {
            return choc::value::createBool(false);
        }

        WidgetBridge::NativeGpuBridgeState::CanvasContextState* target_canvas_state = nullptr;
        WidgetBridge::NativeGpuBridgeState::TextureState* target_texture_state = nullptr;
        if (!canvas_id.empty()) {
            auto it = self.native_gpu_bridge_state_->canvases.find(canvas_id);
            if (it == self.native_gpu_bridge_state_->canvases.end() || !it->second.configured || !it->second.texture) {
                return choc::value::createBool(false);
            }
            target_canvas_state = &it->second;
        } else {
            auto it = self.native_gpu_bridge_state_->textures.find(target_texture_id);
            if (it == self.native_gpu_bridge_state_->textures.end() || !it->second.configured || !it->second.texture) {
                return choc::value::createBool(false);
            }
            target_texture_state = &it->second;
        }

        auto* device_ptr = static_cast<wgpu::Device*>(self.gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(self.gpu_surface_->dawn_queue_handle());
        if (device_ptr == nullptr || queue_ptr == nullptr || !(*device_ptr) || !(*queue_ptr)) {
            return choc::value::createBool(false);
        }

        wgpu::ShaderSourceWGSL vertex_wgsl{};
        vertex_wgsl.code = vertex_code.c_str();
        wgpu::ShaderModuleDescriptor vertex_module_desc{};
        vertex_module_desc.nextInChain = &vertex_wgsl;
        auto vertex_module = device_ptr->CreateShaderModule(&vertex_module_desc);
        if (!vertex_module) return choc::value::createBool(false);

        wgpu::ShaderSourceWGSL fragment_wgsl{};
        fragment_wgsl.code = fragment_code.c_str();
        wgpu::ShaderModuleDescriptor fragment_module_desc{};
        fragment_module_desc.nextInChain = &fragment_wgsl;
        auto fragment_module = device_ptr->CreateShaderModule(&fragment_module_desc);
        if (!fragment_module) return choc::value::createBool(false);

        auto vertex_buffers_view = payload["vertexBuffers"];
        uint32_t max_slot = 0;
        for (uint32_t i = 0; i < vertex_buffers_view.size(); ++i) {
            if (vertex_buffers_view[i].hasObjectMember("slot")) {
                max_slot = std::max<uint32_t>(max_slot, static_cast<uint32_t>(std::max(0, vertex_buffers_view[i]["slot"].getWithDefault<int32_t>(0))));
            }
        }

        std::vector<std::vector<wgpu::VertexAttribute>> attribute_storage(max_slot + 1);
        std::vector<wgpu::VertexBufferLayout> vertex_layouts(max_slot + 1);
        std::vector<wgpu::Buffer> vertex_gpu_buffers(max_slot + 1);
        std::vector<bool> vertex_buffer_present(max_slot + 1, false);

        for (uint32_t i = 0; i < vertex_buffers_view.size(); ++i) {
            auto buffer_view = vertex_buffers_view[i];
            auto slot = static_cast<uint32_t>(std::max(0, buffer_view.hasObjectMember("slot") ? buffer_view["slot"].getWithDefault<int32_t>(0) : 0));
            auto array_stride = static_cast<uint64_t>(std::max<int64_t>(0, buffer_view.hasObjectMember("arrayStride") ? buffer_view["arrayStride"].getWithDefault<int64_t>(0) : 0));
            auto step_mode = buffer_view.hasObjectMember("stepMode") ? buffer_view["stepMode"].getWithDefault<std::string>("vertex") : "vertex";
            auto data = buffer_view.hasObjectMember("data") ? json_bytes_to_vector(buffer_view["data"]) : std::vector<uint8_t>{};
            if (data.empty()) {
                return choc::value::createBool(false);
            }
            auto upload_data = pad_webgpu_write_bytes(data);

            auto& attributes = attribute_storage[slot];
            if (buffer_view.hasObjectMember("attributes") && buffer_view["attributes"].isArray()) {
                auto attributes_view = buffer_view["attributes"];
                attributes.reserve(attributes_view.size());
                for (uint32_t j = 0; j < attributes_view.size(); ++j) {
                    auto attribute_view = attributes_view[j];
                    wgpu::VertexAttribute attribute{};
                    attribute.shaderLocation = static_cast<uint32_t>(std::max(0, attribute_view.hasObjectMember("shaderLocation") ? attribute_view["shaderLocation"].getWithDefault<int32_t>(0) : 0));
                    attribute.offset = static_cast<uint64_t>(std::max<int64_t>(0, attribute_view.hasObjectMember("offset") ? attribute_view["offset"].getWithDefault<int64_t>(0) : 0));
                    attribute.format = vertex_format_from_string(attribute_view.hasObjectMember("format") ? attribute_view["format"].getWithDefault<std::string>("float32x2") : "float32x2");
                    attributes.push_back(attribute);
                }
            }

            wgpu::BufferDescriptor buffer_desc{};
            buffer_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
            buffer_desc.size = upload_data.size();
            auto gpu_buffer = device_ptr->CreateBuffer(&buffer_desc);
            if (!gpu_buffer) return choc::value::createBool(false);
#ifdef PULP_BENCHMARK
            {
                const double t0 = render::bench::now_us();
                queue_ptr->WriteBuffer(gpu_buffer, 0, upload_data.data(), upload_data.size());
                if (self.bench_counters_) {
                    self.bench_counters_->gpu_upload_total_us.fetch_add(
                        render::bench::now_us() - t0,
                        std::memory_order_relaxed);
                    self.bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                        static_cast<double>(upload_data.size()),
                        std::memory_order_relaxed);
                    self.bench_counters_->gpu_buffer_upload_count.fetch_add(
                        1.0, std::memory_order_relaxed);
                    self.bench_counters_->observe_resident_peak(
                        static_cast<double>(upload_data.size()));
                }
            }
#else
            queue_ptr->WriteBuffer(gpu_buffer, 0, upload_data.data(), upload_data.size());
#endif

            vertex_gpu_buffers[slot] = gpu_buffer;
            vertex_buffer_present[slot] = true;
            vertex_layouts[slot].arrayStride = array_stride;
            vertex_layouts[slot].stepMode = vertex_step_mode_from_string(step_mode);
            vertex_layouts[slot].attributeCount = attributes.size();
            vertex_layouts[slot].attributes = attributes.empty() ? nullptr : attributes.data();
        }

        std::vector<uint32_t> bind_group_indices;
        std::vector<wgpu::Buffer> bind_group_buffers;
        std::vector<wgpu::Sampler> bind_group_samplers;
        std::vector<wgpu::Texture> bind_group_textures;
        std::vector<wgpu::TextureView> bind_group_texture_views;
        std::vector<wgpu::BindGroup> bind_groups;
        // (group_index, entries) captured during serialization and turned into
        // bind groups AFTER the pipeline is built with an auto layout - see the
        // deferred-creation comment below.
        std::vector<std::pair<uint32_t, std::vector<wgpu::BindGroupEntry>>> deferred_bind_groups;
        if (payload.hasObjectMember("bindGroups") && payload["bindGroups"].isArray()) {
            auto bind_groups_payload = payload["bindGroups"];
            bind_group_indices.reserve(bind_groups_payload.size());
            deferred_bind_groups.reserve(bind_groups_payload.size());
            bind_groups.reserve(bind_groups_payload.size());

            for (uint32_t i = 0; i < bind_groups_payload.size(); ++i) {
                auto bind_group_view = bind_groups_payload[i];
                if (!bind_group_view.isObject() || !bind_group_view.hasObjectMember("entries") || !bind_group_view["entries"].isArray()) {
                    return choc::value::createBool(false);
                }

                auto group_index = static_cast<uint32_t>(std::max(0, bind_group_view.hasObjectMember("index")
                    ? bind_group_view["index"].getWithDefault<int32_t>(0)
                    : 0));
                auto entries_view = bind_group_view["entries"];
                std::vector<wgpu::BindGroupLayoutEntry> bind_group_layout_entries;
                bind_group_layout_entries.reserve(entries_view.size());
                std::vector<wgpu::BindGroupEntry> bind_group_entries;
                bind_group_entries.reserve(entries_view.size());

                for (uint32_t j = 0; j < entries_view.size(); ++j) {
                    auto entry_view = entries_view[j];
                    if (!entry_view.isObject()) {
                        return choc::value::createBool(false);
                    }

                    auto resource_type = entry_view.hasObjectMember("resourceType")
                        ? entry_view["resourceType"].getWithDefault<std::string>("buffer")
                        : "buffer";
                    auto binding = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("binding")
                        ? entry_view["binding"].getWithDefault<int32_t>(0)
                        : 0));

                    wgpu::BindGroupLayoutEntry bind_group_layout_entry{};
                    bind_group_layout_entry.binding = binding;
                    bind_group_layout_entry.visibility = static_cast<wgpu::ShaderStage>(std::max(0, entry_view.hasObjectMember("visibility")
                        ? entry_view["visibility"].getWithDefault<int32_t>(static_cast<int32_t>(wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment))
                        : static_cast<int32_t>(wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment)));

                    wgpu::BindGroupEntry bind_group_entry{};
                    bind_group_entry.binding = binding;

                    if (resource_type == "buffer") {
                        auto buffer_type = buffer_binding_type_from_string(entry_view.hasObjectMember("bufferType")
                            ? entry_view["bufferType"].getWithDefault<std::string>("uniform")
                            : "uniform");
                        auto bytes = entry_view.hasObjectMember("data")
                            ? json_bytes_to_vector(entry_view["data"])
                            : std::vector<uint8_t>{};
                        auto upload_data = pad_webgpu_write_bytes(std::move(bytes));
                        if (upload_data.empty()) {
                            return choc::value::createBool(false);
                        }

                        auto binding_size = static_cast<uint64_t>(std::max<int64_t>(0, entry_view.hasObjectMember("size")
                            ? entry_view["size"].getWithDefault<int64_t>(static_cast<int64_t>(upload_data.size()))
                            : static_cast<int64_t>(upload_data.size())));
                        if (binding_size == 0) {
                            binding_size = upload_data.size();
                        }

                        bind_group_layout_entry.buffer.type = buffer_type;
                        bind_group_layout_entry.buffer.hasDynamicOffset = entry_view.hasObjectMember("hasDynamicOffset")
                            ? entry_view["hasDynamicOffset"].getWithDefault<bool>(false)
                            : false;
                        bind_group_layout_entry.buffer.minBindingSize = static_cast<uint64_t>(std::max<int64_t>(0, entry_view.hasObjectMember("minBindingSize")
                            ? entry_view["minBindingSize"].getWithDefault<int64_t>(static_cast<int64_t>(binding_size))
                            : static_cast<int64_t>(binding_size)));

                        wgpu::BufferDescriptor buffer_desc{};
                        buffer_desc.usage = buffer_usage_for_binding_type(buffer_type);
                        buffer_desc.size = upload_data.size();
                        auto gpu_buffer = device_ptr->CreateBuffer(&buffer_desc);
                        if (!gpu_buffer) {
                            return choc::value::createBool(false);
                        }
#ifdef PULP_BENCHMARK
                        {
                            const double t0 = render::bench::now_us();
                            queue_ptr->WriteBuffer(gpu_buffer, 0, upload_data.data(), upload_data.size());
                            if (self.bench_counters_) {
                                self.bench_counters_->gpu_upload_total_us.fetch_add(
                                    render::bench::now_us() - t0,
                                    std::memory_order_relaxed);
                                self.bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                                    static_cast<double>(upload_data.size()),
                                    std::memory_order_relaxed);
                                self.bench_counters_->gpu_buffer_upload_count.fetch_add(
                                    1.0, std::memory_order_relaxed);
                                self.bench_counters_->observe_resident_peak(
                                    static_cast<double>(upload_data.size()));
                            }
                        }
#else
                        queue_ptr->WriteBuffer(gpu_buffer, 0, upload_data.data(), upload_data.size());
#endif
                        bind_group_buffers.push_back(gpu_buffer);

                        bind_group_entry.buffer = gpu_buffer;
                        bind_group_entry.offset = 0;
                        bind_group_entry.size = binding_size;
                        bind_group_layout_entries.push_back(bind_group_layout_entry);
                        bind_group_entries.push_back(bind_group_entry);
                        continue;
                    }

                    if (resource_type == "sampler") {
                        bind_group_layout_entry.sampler.type = wgpu::SamplerBindingType::Filtering;
                        wgpu::SamplerDescriptor sampler_desc{};
                        sampler_desc.addressModeU = address_mode_from_string(entry_view.hasObjectMember("addressModeU")
                            ? entry_view["addressModeU"].getWithDefault<std::string>("clamp-to-edge")
                            : "clamp-to-edge");
                        sampler_desc.addressModeV = address_mode_from_string(entry_view.hasObjectMember("addressModeV")
                            ? entry_view["addressModeV"].getWithDefault<std::string>("clamp-to-edge")
                            : "clamp-to-edge");
                        sampler_desc.addressModeW = address_mode_from_string(entry_view.hasObjectMember("addressModeW")
                            ? entry_view["addressModeW"].getWithDefault<std::string>("clamp-to-edge")
                            : "clamp-to-edge");
                        sampler_desc.magFilter = filter_mode_from_string(entry_view.hasObjectMember("magFilter")
                            ? entry_view["magFilter"].getWithDefault<std::string>("nearest")
                            : "nearest");
                        sampler_desc.minFilter = filter_mode_from_string(entry_view.hasObjectMember("minFilter")
                            ? entry_view["minFilter"].getWithDefault<std::string>("nearest")
                            : "nearest");
                        sampler_desc.mipmapFilter = mipmap_filter_mode_from_string(entry_view.hasObjectMember("mipmapFilter")
                            ? entry_view["mipmapFilter"].getWithDefault<std::string>("nearest")
                            : "nearest");
                        auto sampler = device_ptr->CreateSampler(&sampler_desc);
                        if (!sampler) {
                            return choc::value::createBool(false);
                        }
                        bind_group_samplers.push_back(sampler);
                        bind_group_entry.sampler = sampler;
                        bind_group_layout_entries.push_back(bind_group_layout_entry);
                        bind_group_entries.push_back(bind_group_entry);
                        continue;
                    }

                    if (resource_type == "textureView") {
                        bind_group_layout_entry.texture.sampleType = wgpu::TextureSampleType::Float;
                        bind_group_layout_entry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
                        bind_group_layout_entry.texture.multisampled = false;

                        auto source_texture_id = entry_view.hasObjectMember("sourceTextureId")
                            ? entry_view["sourceTextureId"].getWithDefault<std::string>("")
                            : "";
                        auto source_canvas_id = entry_view.hasObjectMember("sourceCanvasId")
                            ? entry_view["sourceCanvasId"].getWithDefault<std::string>("")
                            : "";
                        auto default_view_format = !source_texture_id.empty()
                            ? format
                            : (source_canvas_id.empty() ? format : "bgra8unorm");
                        auto view_format = entry_view.hasObjectMember("format")
                            ? entry_view["format"].getWithDefault<std::string>(default_view_format)
                            : default_view_format;
                        auto view_dimension = entry_view.hasObjectMember("dimension")
                            ? entry_view["dimension"].getWithDefault<std::string>("2d")
                            : "2d";
                        auto view_aspect = entry_view.hasObjectMember("aspect")
                            ? entry_view["aspect"].getWithDefault<std::string>("all")
                            : "all";
                        auto base_mip_level = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("baseMipLevel")
                            ? entry_view["baseMipLevel"].getWithDefault<int32_t>(0)
                            : 0));
                        auto mip_level_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("mipLevelCount")
                            ? entry_view["mipLevelCount"].getWithDefault<int32_t>(1)
                            : 1));
                        auto base_array_layer = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("baseArrayLayer")
                            ? entry_view["baseArrayLayer"].getWithDefault<int32_t>(0)
                            : 0));
                        auto array_layer_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("arrayLayerCount")
                            ? entry_view["arrayLayerCount"].getWithDefault<int32_t>(1)
                            : 1));
                        wgpu::TextureView texture_view;

                        if (!source_texture_id.empty()) {
                            auto source_it = self.native_gpu_bridge_state_->textures.find(source_texture_id);
                            if (source_it == self.native_gpu_bridge_state_->textures.end() ||
                                !source_it->second.configured || !source_it->second.texture) {
                                return choc::value::createBool(false);
                            }

                            const bool use_default_view =
                                view_format == source_it->second.format &&
                                view_dimension == "2d" &&
                                view_aspect == "all" &&
                                base_mip_level == 0 &&
                                mip_level_count == 1 &&
                                base_array_layer == 0 &&
                                array_layer_count == 1;

                            texture_view = use_default_view
                                ? source_it->second.texture.CreateView()
                                : [&]() {
                                    wgpu::TextureViewDescriptor texture_view_desc{};
                                    texture_view_desc.format = texture_format_from_string(view_format);
                                    texture_view_desc.dimension = texture_view_dimension_from_string(view_dimension);
                                    texture_view_desc.aspect = texture_aspect_from_string(view_aspect);
                                    texture_view_desc.baseMipLevel = base_mip_level;
                                    texture_view_desc.mipLevelCount = mip_level_count;
                                    texture_view_desc.baseArrayLayer = base_array_layer;
                                    texture_view_desc.arrayLayerCount = array_layer_count;
                                    return source_it->second.texture.CreateView(&texture_view_desc);
                                }();
                        } else if (!source_canvas_id.empty()) {
                            auto source_it = self.native_gpu_bridge_state_->canvases.find(source_canvas_id);
                            if (source_it == self.native_gpu_bridge_state_->canvases.end() ||
                                !source_it->second.configured || !source_it->second.texture) {
                                return choc::value::createBool(false);
                            }

                            const bool use_default_view =
                                view_format == source_it->second.format &&
                                view_dimension == "2d" &&
                                view_aspect == "all" &&
                                base_mip_level == 0 &&
                                mip_level_count == 1 &&
                                base_array_layer == 0 &&
                                array_layer_count == 1;

                            texture_view = use_default_view
                                ? source_it->second.texture.CreateView()
                                : [&]() {
                                    wgpu::TextureViewDescriptor texture_view_desc{};
                                    texture_view_desc.format = texture_format_from_string(view_format);
                                    texture_view_desc.dimension = texture_view_dimension_from_string(view_dimension);
                                    texture_view_desc.aspect = texture_aspect_from_string(view_aspect);
                                    texture_view_desc.baseMipLevel = base_mip_level;
                                    texture_view_desc.mipLevelCount = mip_level_count;
                                    texture_view_desc.baseArrayLayer = base_array_layer;
                                    texture_view_desc.arrayLayerCount = array_layer_count;
                                    return source_it->second.texture.CreateView(&texture_view_desc);
                                }();
                        } else {
                            auto texture_width = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("width")
                                ? entry_view["width"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_height = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("height")
                                ? entry_view["height"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_depth = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("depthOrArrayLayers")
                                ? entry_view["depthOrArrayLayers"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_usage_mask = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("usage")
                                ? entry_view["usage"].getWithDefault<int32_t>(0)
                                : 0));
                            auto texture_sample_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("sampleCount")
                                ? entry_view["sampleCount"].getWithDefault<int32_t>(1)
                                : 1));
                            auto texture_mip_level_count = static_cast<uint32_t>(std::max(1, entry_view.hasObjectMember("textureMipLevelCount")
                                ? entry_view["textureMipLevelCount"].getWithDefault<int32_t>(1)
                                : 1));
                            auto bytes_per_row = static_cast<uint32_t>(std::max(0, entry_view.hasObjectMember("bytesPerRow")
                                ? entry_view["bytesPerRow"].getWithDefault<int32_t>(0)
                                : 0));
                            auto rows_per_image = static_cast<uint32_t>(std::max<int32_t>(1, entry_view.hasObjectMember("rowsPerImage")
                                ? entry_view["rowsPerImage"].getWithDefault<int32_t>(static_cast<int32_t>(texture_height))
                                : static_cast<int32_t>(texture_height)));
                            auto texture_bytes = entry_view.hasObjectMember("data")
                                ? json_bytes_to_vector(entry_view["data"])
                                : std::vector<uint8_t>{};
                            if (texture_bytes.empty()) {
                                return choc::value::createBool(false);
                            }

                            auto required_bytes_per_row = texture_width * texture_bytes_per_pixel_from_format(view_format);
                            if (bytes_per_row == 0) {
                                bytes_per_row = required_bytes_per_row;
                            }

                            wgpu::TextureDescriptor texture_desc{};
                            texture_desc.dimension = wgpu::TextureDimension::e2D;
                            texture_desc.size.width = texture_width;
                            texture_desc.size.height = texture_height;
                            texture_desc.size.depthOrArrayLayers = texture_depth;
                            texture_desc.format = texture_format_from_string(view_format);
                            texture_desc.usage = texture_usage_from_mask(texture_usage_mask);
                            texture_desc.mipLevelCount = texture_mip_level_count;
                            texture_desc.sampleCount = texture_sample_count;
                            if ((texture_desc.usage & wgpu::TextureUsage::TextureBinding) == wgpu::TextureUsage::None) {
                                texture_desc.usage |= wgpu::TextureUsage::TextureBinding;
                            }
                            if ((texture_desc.usage & wgpu::TextureUsage::CopyDst) == wgpu::TextureUsage::None) {
                                texture_desc.usage |= wgpu::TextureUsage::CopyDst;
                            }

                            auto uploaded_texture = device_ptr->CreateTexture(&texture_desc);
                            if (!uploaded_texture) {
                                return choc::value::createBool(false);
                            }

                            wgpu::TexelCopyTextureInfo destination{};
                            destination.texture = uploaded_texture;
                            destination.aspect = wgpu::TextureAspect::All;
                            wgpu::TexelCopyBufferLayout data_layout{};
                            data_layout.offset = 0;
                            data_layout.bytesPerRow = bytes_per_row;
                            data_layout.rowsPerImage = rows_per_image;
                            wgpu::Extent3D write_size{};
                            write_size.width = texture_width;
                            write_size.height = texture_height;
                            write_size.depthOrArrayLayers = texture_depth;
                            queue_ptr->WriteTexture(&destination, texture_bytes.data(), texture_bytes.size(), &data_layout, &write_size);

                            wgpu::TextureViewDescriptor texture_view_desc{};
                            texture_view_desc.format = texture_format_from_string(view_format);
                            texture_view_desc.dimension = texture_view_dimension_from_string(view_dimension);
                            texture_view_desc.aspect = texture_aspect_from_string(view_aspect);
                            texture_view_desc.baseMipLevel = base_mip_level;
                            texture_view_desc.mipLevelCount = mip_level_count;
                            texture_view_desc.baseArrayLayer = base_array_layer;
                            texture_view_desc.arrayLayerCount = array_layer_count;
                            texture_view = uploaded_texture.CreateView(&texture_view_desc);
                            bind_group_textures.push_back(uploaded_texture);
                        }

                        if (!texture_view) {
                            return choc::value::createBool(false);
                        }
                        bind_group_texture_views.push_back(texture_view);
                        bind_group_entry.textureView = texture_view;
                        bind_group_layout_entries.push_back(bind_group_layout_entry);
                        bind_group_entries.push_back(bind_group_entry);
                        continue;
                    }

                    return choc::value::createBool(false);
                }

                // Defer bind-group creation until the pipeline exists. The JS
                // serializer guesses each entry's visibility/type by
                // regex-scanning the WGSL
                // (web-compat-gpu-buffered.js inferVisibilityFromShaders); an
                // explicit BindGroupLayout built from those guesses can silently
                // diverge from Three.js's `layout:"auto"` pipeline interface.
                // With the Sim's skip_validation toggle that divergence does NOT
                // raise an error - it just leaves the vertex stage reading zeroed
                // uniforms, collapsing the cube to a degenerate point (every
                // matrix/vertex byte verified correct yet no fragments emit).
                // Instead, build each group from pipeline.GetBindGroupLayout()
                // below, exactly as the immediate __gpuQueueDrawImpl path does.
                (void)bind_group_layout_entries;
                deferred_bind_groups.emplace_back(group_index, std::move(bind_group_entries));
            }
        }

        // The pipeline's color attachment format MUST match the actual target
        // texture's format, not the JS-supplied payload `format` field.
        // Three.js may request a bgra8unorm RenderPass while the intermediate
        // texture is rgba8unorm; Metal SoftwareRenderer can silently reject the
        // pipeline when validation is suppressed. Use the target's actual
        // format so the pipeline matches the attachment.
        std::string actual_target_format = target_canvas_state != nullptr
            ? target_canvas_state->format
            : (target_texture_state != nullptr ? target_texture_state->format : format);
        if (format != actual_target_format) {
            static int s_warned = 0;
            if (++s_warned <= 3) {
                runtime::log_info("PULP_WEBGPU_BRIDGE: draw target-format mismatch payload={} actual={} canvas={} texId={}",
                    format, actual_target_format, canvas_id, target_texture_id);
            }
        }
        wgpu::ColorTargetState color_target{};
        color_target.format = texture_format_from_string(actual_target_format);
        // The write mask must be explicit. A defaulted None mask lets the
        // pipeline execute without writing color; the immediate
        // __gpuQueueDrawImpl path sets `writeMask = All`, and the buffered path
        // must match it.
        color_target.writeMask = wgpu::ColorWriteMask::All;

        // For canvas-targeted draws specifically, drop the alpha channel from
        // writeMask so the shader's alpha=0 output (Three.js's WebGPURenderer
        // composite path) doesn't reset the destination alpha to 0. Combined
        // with the loadOp=Clear/alpha=1 override below at color_attachment, this
        // keeps the canvas swapchain alpha at 1 (opaque) regardless of shader
        // output. Skia then composites the RGB content the shader wrote instead
        // of compositing src.alpha=0 against the canvasCard CSS background.
        if (target_canvas_state != nullptr) {
            color_target.writeMask = wgpu::ColorWriteMask::Red
                                   | wgpu::ColorWriteMask::Green
                                   | wgpu::ColorWriteMask::Blue;
        }

        wgpu::FragmentState fragment_state{};
        fragment_state.module = fragment_module;
        fragment_state.entryPoint = fragment_entry.c_str();
        fragment_state.targetCount = 1;
        fragment_state.targets = &color_target;

        wgpu::RenderPipelineDescriptor pipeline_desc{};
        // Always use an AUTO pipeline layout. Bind groups are created below
        // from pipeline.GetBindGroupLayout(group_index), so the layout is
        // derived from the actual shader interface rather than the JS-side
        // guessed layout (see deferred_bind_groups above).
        pipeline_desc.layout = nullptr;
        pipeline_desc.vertex.module = vertex_module;
        pipeline_desc.vertex.entryPoint = vertex_entry.c_str();
        pipeline_desc.vertex.bufferCount = vertex_layouts.size();
        pipeline_desc.vertex.buffers = vertex_layouts.data();
        pipeline_desc.fragment = &fragment_state;
        pipeline_desc.primitive.topology = primitive_topology_from_string(topology);
        pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;
        pipeline_desc.primitive.cullMode = wgpu::CullMode::None;
        pipeline_desc.multisample.count = 1;
        pipeline_desc.multisample.mask = ~0u;
        pipeline_desc.multisample.alphaToCoverageEnabled = false;

        // Add depth/stencil to pipeline if JS payload requests it
        wgpu::DepthStencilState depth_stencil_state{};
        bool has_depth = payload.hasObjectMember("depthStencil") || payload.hasObjectMember("pipelineDepthStencil");
        if (has_depth) {
            depth_stencil_state.format = wgpu::TextureFormat::Depth24Plus;
            depth_stencil_state.depthWriteEnabled = true;
            depth_stencil_state.depthCompare = wgpu::CompareFunction::Less;
            if (payload.hasObjectMember("pipelineDepthStencil") && payload["pipelineDepthStencil"].isObject()) {
                auto ds = payload["pipelineDepthStencil"];
                auto cmp = ds.hasObjectMember("depthCompare") ? ds["depthCompare"].getWithDefault<std::string>("less") : "less";
                if (cmp == "less-equal") depth_stencil_state.depthCompare = wgpu::CompareFunction::LessEqual;
                else if (cmp == "greater") depth_stencil_state.depthCompare = wgpu::CompareFunction::Greater;
                else if (cmp == "always") depth_stencil_state.depthCompare = wgpu::CompareFunction::Always;
                depth_stencil_state.depthWriteEnabled = ds.hasObjectMember("depthWriteEnabled") ? ds["depthWriteEnabled"].getWithDefault<bool>(true) : true;
            }
            pipeline_desc.depthStencil = &depth_stencil_state;
        }

        auto pipeline = device_ptr->CreateRenderPipeline(&pipeline_desc);
        if (!pipeline) return choc::value::createBool(false);

        // Now that the pipeline (auto layout) exists, build each bind group
        // against the layout Dawn derived from the shader interface. This
        // guarantees the binding visibility/types match what the WGSL actually
        // declares, instead of the JS-side guessed layout that left the vertex
        // stage reading zeroed uniforms on the Simulator.
        for (auto& dg : deferred_bind_groups) {
            wgpu::BindGroupDescriptor bind_group_desc{};
            bind_group_desc.layout = pipeline.GetBindGroupLayout(dg.first);
            bind_group_desc.entryCount = dg.second.size();
            bind_group_desc.entries = dg.second.data();
            auto bind_group = device_ptr->CreateBindGroup(&bind_group_desc);
            if (!bind_group) return choc::value::createBool(false);
            bind_group_indices.push_back(dg.first);
            bind_groups.push_back(bind_group);
        }

        auto texture_view = target_canvas_state != nullptr
            ? target_canvas_state->texture.CreateView()
            : target_texture_state->texture.CreateView();
        if (!texture_view) return choc::value::createBool(false);

        wgpu::RenderPassColorAttachment color_attachment{};
        color_attachment.view = texture_view;
        color_attachment.loadOp = load_op == "clear" ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
        color_attachment.storeOp = store_op == "discard" ? wgpu::StoreOp::Discard : wgpu::StoreOp::Store;
        if (color_attachment.loadOp == wgpu::LoadOp::Clear && payload.hasObjectMember("clearValue") && payload["clearValue"].isObject()) {
            auto clear_value = payload["clearValue"];
            color_attachment.clearValue = {
                static_cast<float>(clear_value.hasObjectMember("r") ? clear_value["r"].getWithDefault<double>(0.0) : 0.0),
                static_cast<float>(clear_value.hasObjectMember("g") ? clear_value["g"].getWithDefault<double>(0.0) : 0.0),
                static_cast<float>(clear_value.hasObjectMember("b") ? clear_value["b"].getWithDefault<double>(0.0) : 0.0),
                static_cast<float>(clear_value.hasObjectMember("a") ? clear_value["a"].getWithDefault<double>(1.0) : 1.0)
            };
        }
        // For canvas-targeted draws, force loadOp=Clear with alpha=1 so the
        // destination alpha starts opaque. Combined with the canvas-specific
        // writeMask (RGB only, set above on the pipeline's color_target), this
        // keeps the canvas swapchain alpha at 1 across all draws regardless of
        // what alpha the shader emits. Without this, Three.js's composite
        // (which writes alpha=0 for the canvas pass) leaves the canvas
        // transparent and the canvasCard CSS background shows through.
        if (target_canvas_state != nullptr) {
            color_attachment.loadOp = wgpu::LoadOp::Clear;
            color_attachment.clearValue.a = 1.0f;
        }

        // Create depth texture if depth/stencil is requested
        wgpu::Texture depth_texture;
        wgpu::TextureView depth_view;
        wgpu::RenderPassDepthStencilAttachment depth_attachment{};
        has_depth = payload.hasObjectMember("depthStencil") || payload.hasObjectMember("pipelineDepthStencil");

        if (has_depth) {
            wgpu::TextureDescriptor depth_tex_desc{};
            uint32_t depth_w = target_canvas_state ? target_canvas_state->width : (target_texture_state ? target_texture_state->width : 256);
            uint32_t depth_h = target_canvas_state ? target_canvas_state->height : (target_texture_state ? target_texture_state->height : 256);
            depth_tex_desc.size = {depth_w, depth_h, 1};
            depth_tex_desc.format = wgpu::TextureFormat::Depth24Plus;
            depth_tex_desc.usage = wgpu::TextureUsage::RenderAttachment;
            depth_texture = device_ptr->CreateTexture(&depth_tex_desc);
            if (depth_texture) {
                depth_view = depth_texture.CreateView();
                depth_attachment.view = depth_view;
                depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
                depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
                depth_attachment.depthClearValue = 1.0f;
            } else {
                has_depth = false;
            }
        }

        wgpu::RenderPassDescriptor pass_desc{};
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color_attachment;
        if (has_depth) {
            pass_desc.depthStencilAttachment = &depth_attachment;
        }

        wgpu::CommandEncoderDescriptor encoder_desc{};
        auto encoder = device_ptr->CreateCommandEncoder(&encoder_desc);
        if (!encoder) return choc::value::createBool(false);

        auto pass = encoder.BeginRenderPass(&pass_desc);
        pass.SetPipeline(pipeline);
        for (size_t i = 0; i < bind_groups.size(); ++i) {
            pass.SetBindGroup(bind_group_indices[i], bind_groups[i], 0, nullptr);
        }
        for (uint32_t slot = 0; slot < vertex_gpu_buffers.size(); ++slot) {
            if (vertex_buffer_present[slot]) {
                pass.SetVertexBuffer(slot, vertex_gpu_buffers[slot]);
            }
        }

        if (draw_type == "draw-indexed") {
            if (!payload.hasObjectMember("indexBuffer")) {
                return choc::value::createBool(false);
            }
            auto index_buffer_view = payload["indexBuffer"];
            auto index_data = index_buffer_view.hasObjectMember("data") ? json_bytes_to_vector(index_buffer_view["data"]) : std::vector<uint8_t>{};
            if (index_data.empty()) {
                return choc::value::createBool(false);
            }
            auto upload_index_data = pad_webgpu_write_bytes(index_data);
            wgpu::BufferDescriptor index_buffer_desc{};
            index_buffer_desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
            index_buffer_desc.size = upload_index_data.size();
            auto index_gpu_buffer = device_ptr->CreateBuffer(&index_buffer_desc);
            if (!index_gpu_buffer) return choc::value::createBool(false);
#ifdef PULP_BENCHMARK
            {
                const double t0 = render::bench::now_us();
                queue_ptr->WriteBuffer(index_gpu_buffer, 0, upload_index_data.data(), upload_index_data.size());
                if (self.bench_counters_) {
                    self.bench_counters_->gpu_upload_total_us.fetch_add(
                        render::bench::now_us() - t0,
                        std::memory_order_relaxed);
                    self.bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                        static_cast<double>(upload_index_data.size()),
                        std::memory_order_relaxed);
                    self.bench_counters_->gpu_buffer_upload_count.fetch_add(
                        1.0, std::memory_order_relaxed);
                    self.bench_counters_->observe_resident_peak(
                        static_cast<double>(upload_index_data.size()));
                }
            }
#else
            queue_ptr->WriteBuffer(index_gpu_buffer, 0, upload_index_data.data(), upload_index_data.size());
#endif
            pass.SetIndexBuffer(index_gpu_buffer, index_format_from_string(index_buffer_view.hasObjectMember("format") ? index_buffer_view["format"].getWithDefault<std::string>("uint32") : "uint32"));

            auto index_count = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("indexCount") ? payload["indexCount"].getWithDefault<int32_t>(0) : 0));
            auto instance_count = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("instanceCount") ? payload["instanceCount"].getWithDefault<int32_t>(1) : 1));
            auto first_index = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("firstIndex") ? payload["firstIndex"].getWithDefault<int32_t>(0) : 0));
            auto base_vertex = static_cast<int32_t>(payload.hasObjectMember("baseVertex") ? payload["baseVertex"].getWithDefault<int32_t>(0) : 0);
            auto first_instance = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("firstInstance") ? payload["firstInstance"].getWithDefault<int32_t>(0) : 0));
            if (index_count == 0) return choc::value::createBool(false);
            if (first_instance > 0 && device_ptr->HasFeature(wgpu::FeatureName::IndirectFirstInstance)) {
                runtime::log_info(
                    "PULP_WEBGPU_BRIDGE: DrawIndexedIndirect/buffered firstInstance={} indexCount={} instanceCount={}",
                    first_instance, index_count, instance_count);
                uint32_t indirect_args[5] = { index_count, instance_count, first_index,
                                              static_cast<uint32_t>(base_vertex), first_instance };
                wgpu::BufferDescriptor ibd{};
                ibd.size = sizeof(indirect_args);
                ibd.usage = wgpu::BufferUsage::Indirect | wgpu::BufferUsage::CopyDst;
                auto ibuf = device_ptr->CreateBuffer(&ibd);
                queue_ptr->WriteBuffer(ibuf, 0, indirect_args, sizeof(indirect_args));
                pass.DrawIndexedIndirect(ibuf, 0);
            } else {
                pass.DrawIndexed(index_count, instance_count, first_index, base_vertex, first_instance);
            }
        } else {
            auto vertex_count = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("vertexCount") ? payload["vertexCount"].getWithDefault<int32_t>(0) : 0));
            auto instance_count = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("instanceCount") ? payload["instanceCount"].getWithDefault<int32_t>(1) : 1));
            auto first_vertex = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("firstVertex") ? payload["firstVertex"].getWithDefault<int32_t>(0) : 0));
            auto first_instance = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("firstInstance") ? payload["firstInstance"].getWithDefault<int32_t>(0) : 0));
            if (vertex_count == 0) return choc::value::createBool(false);
            if (first_instance > 0 && device_ptr->HasFeature(wgpu::FeatureName::IndirectFirstInstance)) {
                runtime::log_info(
                    "PULP_WEBGPU_BRIDGE: DrawIndirect/buffered firstInstance={} vertexCount={} instanceCount={}",
                    first_instance, vertex_count, instance_count);
                uint32_t indirect_args[4] = { vertex_count, instance_count, first_vertex, first_instance };
                wgpu::BufferDescriptor ibd{};
                ibd.size = sizeof(indirect_args);
                ibd.usage = wgpu::BufferUsage::Indirect | wgpu::BufferUsage::CopyDst;
                auto ibuf = device_ptr->CreateBuffer(&ibd);
                queue_ptr->WriteBuffer(ibuf, 0, indirect_args, sizeof(indirect_args));
                pass.DrawIndirect(ibuf, 0);
            } else {
                pass.Draw(vertex_count, instance_count, first_vertex, first_instance);
            }
        }

        pass.End();
        auto command_buffer = encoder.Finish();
        queue_ptr->Submit(1, &command_buffer);
        if (target_canvas_state != nullptr) {
            self.request_repaint();
        }
        return choc::value::createBool(true);
#endif
    });

    register_bridge_function(api, "__gpuQueuePresentTextureImpl", [&self](choc::javascript::ArgumentList args) {
        if (args.numArgs < 1 || !args[0] || self.native_gpu_bridge_state_ == nullptr || self.gpu_surface_ == nullptr) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createBool(false);
#else
        auto& payload = *args[0];
        if (!payload.isObject()) {
            return choc::value::createBool(false);
        }

        auto canvas_id = payload.hasObjectMember("canvasId")
            ? payload["canvasId"].getWithDefault<std::string>("")
            : "";
        auto source_texture_id = payload.hasObjectMember("sourceTextureId")
            ? payload["sourceTextureId"].getWithDefault<std::string>("")
            : "";
        if (canvas_id.empty() || source_texture_id.empty()) {
            return choc::value::createBool(false);
        }

        auto canvas_it = self.native_gpu_bridge_state_->canvases.find(canvas_id);
        auto source_it = self.native_gpu_bridge_state_->textures.find(source_texture_id);
        if (canvas_it == self.native_gpu_bridge_state_->canvases.end() ||
            source_it == self.native_gpu_bridge_state_->textures.end() ||
            !canvas_it->second.configured || !canvas_it->second.texture ||
            !source_it->second.configured || !source_it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* device_ptr = static_cast<wgpu::Device*>(self.gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(self.gpu_surface_->dawn_queue_handle());
        if (device_ptr == nullptr || queue_ptr == nullptr || !(*device_ptr) || !(*queue_ptr)) {
            return choc::value::createBool(false);
        }

        static constexpr const char* kFullscreenBlitVertex = R"WGSL(
struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(0) uv : vec2<f32>
};

@vertex
fn main(@builtin(vertex_index) vertex_index : u32) -> VertexOut {
    var positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -3.0),
        vec2<f32>(-1.0, 1.0),
        vec2<f32>(3.0, 1.0)
    );
    var uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 2.0),
        vec2<f32>(0.0, 0.0),
        vec2<f32>(2.0, 0.0)
    );

    var out : VertexOut;
    out.position = vec4<f32>(positions[vertex_index], 0.0, 1.0);
    out.uv = uvs[vertex_index];
    return out;
}
)WGSL";

        static constexpr const char* kFullscreenBlitFragment = R"WGSL(
@group(0) @binding(0) var sourceSampler : sampler;
@group(0) @binding(1) var sourceTexture : texture_2d<f32>;

@fragment
fn main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    return textureSample(sourceTexture, sourceSampler, uv);
}
)WGSL";

        wgpu::ShaderSourceWGSL vertex_wgsl{};
        vertex_wgsl.code = kFullscreenBlitVertex;
        wgpu::ShaderModuleDescriptor vertex_desc{};
        vertex_desc.nextInChain = &vertex_wgsl;
        auto vertex_module = device_ptr->CreateShaderModule(&vertex_desc);
        if (!vertex_module) return choc::value::createBool(false);

        wgpu::ShaderSourceWGSL fragment_wgsl{};
        fragment_wgsl.code = kFullscreenBlitFragment;
        wgpu::ShaderModuleDescriptor fragment_desc{};
        fragment_desc.nextInChain = &fragment_wgsl;
        auto fragment_module = device_ptr->CreateShaderModule(&fragment_desc);
        if (!fragment_module) return choc::value::createBool(false);

        wgpu::BindGroupLayoutEntry sampler_layout{};
        sampler_layout.binding = 0;
        sampler_layout.visibility = wgpu::ShaderStage::Fragment;
        sampler_layout.sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutEntry texture_layout{};
        texture_layout.binding = 1;
        texture_layout.visibility = wgpu::ShaderStage::Fragment;
        texture_layout.texture.sampleType = wgpu::TextureSampleType::Float;
        texture_layout.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        texture_layout.texture.multisampled = false;

        std::array<wgpu::BindGroupLayoutEntry, 2> bind_group_layout_entries{ sampler_layout, texture_layout };
        wgpu::BindGroupLayoutDescriptor bind_group_layout_desc{};
        bind_group_layout_desc.entryCount = bind_group_layout_entries.size();
        bind_group_layout_desc.entries = bind_group_layout_entries.data();
        auto bind_group_layout = device_ptr->CreateBindGroupLayout(&bind_group_layout_desc);
        if (!bind_group_layout) return choc::value::createBool(false);

        wgpu::PipelineLayoutDescriptor pipeline_layout_desc{};
        pipeline_layout_desc.bindGroupLayoutCount = 1;
        pipeline_layout_desc.bindGroupLayouts = &bind_group_layout;
        auto pipeline_layout = device_ptr->CreatePipelineLayout(&pipeline_layout_desc);
        if (!pipeline_layout) return choc::value::createBool(false);

        wgpu::ColorTargetState color_target{};
        color_target.format = texture_format_from_string(canvas_it->second.format);
        color_target.writeMask = wgpu::ColorWriteMask::All;

        wgpu::FragmentState fragment_state{};
        fragment_state.module = fragment_module;
        fragment_state.entryPoint = "main";
        fragment_state.targetCount = 1;
        fragment_state.targets = &color_target;

        wgpu::RenderPipelineDescriptor pipeline_desc{};
        pipeline_desc.layout = pipeline_layout;
        pipeline_desc.vertex.module = vertex_module;
        pipeline_desc.vertex.entryPoint = "main";
        pipeline_desc.vertex.bufferCount = 0;
        pipeline_desc.vertex.buffers = nullptr;
        pipeline_desc.fragment = &fragment_state;
        pipeline_desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;
        pipeline_desc.primitive.cullMode = wgpu::CullMode::None;
        pipeline_desc.multisample.count = 1;
        pipeline_desc.multisample.mask = ~0u;
        pipeline_desc.multisample.alphaToCoverageEnabled = false;
        auto pipeline = device_ptr->CreateRenderPipeline(&pipeline_desc);
        if (!pipeline) return choc::value::createBool(false);

        wgpu::SamplerDescriptor sampler_desc{};
        sampler_desc.addressModeU = wgpu::AddressMode::ClampToEdge;
        sampler_desc.addressModeV = wgpu::AddressMode::ClampToEdge;
        sampler_desc.addressModeW = wgpu::AddressMode::ClampToEdge;
        sampler_desc.magFilter = wgpu::FilterMode::Linear;
        sampler_desc.minFilter = wgpu::FilterMode::Linear;
        sampler_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        auto sampler = device_ptr->CreateSampler(&sampler_desc);
        if (!sampler) return choc::value::createBool(false);

        auto source_texture_view = source_it->second.texture.CreateView();
        auto destination_texture_view = canvas_it->second.texture.CreateView();
        if (!source_texture_view || !destination_texture_view) {
            return choc::value::createBool(false);
        }

        std::array<wgpu::BindGroupEntry, 2> bind_group_entries{};
        bind_group_entries[0].binding = 0;
        bind_group_entries[0].sampler = sampler;
        bind_group_entries[1].binding = 1;
        bind_group_entries[1].textureView = source_texture_view;

        wgpu::BindGroupDescriptor bind_group_desc{};
        bind_group_desc.layout = bind_group_layout;
        bind_group_desc.entryCount = bind_group_entries.size();
        bind_group_desc.entries = bind_group_entries.data();
        auto bind_group = device_ptr->CreateBindGroup(&bind_group_desc);
        if (!bind_group) return choc::value::createBool(false);

        wgpu::RenderPassColorAttachment color_attachment{};
        color_attachment.view = destination_texture_view;
        color_attachment.loadOp = wgpu::LoadOp::Clear;
        color_attachment.storeOp = wgpu::StoreOp::Store;
        color_attachment.clearValue = {0.0, 0.0, 0.0, 1.0};

        wgpu::RenderPassDescriptor pass_desc{};
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color_attachment;

        wgpu::CommandEncoderDescriptor encoder_desc{};
        auto encoder = device_ptr->CreateCommandEncoder(&encoder_desc);
        if (!encoder) return choc::value::createBool(false);

        auto pass = encoder.BeginRenderPass(&pass_desc);
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bind_group, 0, nullptr);
        pass.Draw(3, 1, 0, 0);
        pass.End();

        auto command_buffer = encoder.Finish();
        queue_ptr->Submit(1, &command_buffer);
        self.request_repaint();
        return choc::value::createBool(true);
#endif
    });

    register_bridge_function(api, "__gpuCanvasPresentImpl", [&self](choc::javascript::ArgumentList args) {
        auto canvas_id = args.get<std::string>(0, "");
        if (canvas_id.empty() || self.native_gpu_bridge_state_ == nullptr) {
            return choc::value::createBool(false);
        }

        auto it = self.native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == self.native_gpu_bridge_state_->canvases.end()) {
            return choc::value::createBool(false);
        }

        if (it->second.configured) {
            self.request_repaint();
        }
        return choc::value::createBool(it->second.configured);
    });

    register_bridge_promise_function(api, "__requestAdapterImpl", [&self](const choc::value::Value*, size_t) {
        return detail::gpu_descriptor_to_value(detail::widget_bridge_gpu_info(self.gpu_surface_));
    });

    // -- Compute pipeline dispatch ---------------------------------------
    // Receives JSON from the JS compute pass encoder and dispatches
    // via Dawn's native compute pipeline infrastructure.
    register_bridge_function(api, "__gpuComputeDispatchImpl", [&self](choc::javascript::ArgumentList args) {
        if (args.numArgs < 1 || !args[0] || self.gpu_surface_ == nullptr) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createBool(false);
#else
        auto payload_str = args.get<std::string>(0, "");
        if (payload_str.empty()) return choc::value::createBool(false);

        auto* device_ptr = static_cast<wgpu::Device*>(self.gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(self.gpu_surface_->dawn_queue_handle());
        if (!device_ptr || !queue_ptr || !(*device_ptr) || !(*queue_ptr))
            return choc::value::createBool(false);

        try {
            auto payload = choc::json::parse(payload_str);
            auto shader_code = payload.hasObjectMember("shaderCode")
                ? payload["shaderCode"].getWithDefault<std::string>("") : "";
            auto entry_point = payload.hasObjectMember("entryPoint")
                ? payload["entryPoint"].getWithDefault<std::string>("main") : "main";
            auto wg_x = static_cast<uint32_t>(payload.hasObjectMember("workgroupCountX")
                ? payload["workgroupCountX"].getWithDefault<int64_t>(1) : 1);
            auto wg_y = static_cast<uint32_t>(payload.hasObjectMember("workgroupCountY")
                ? payload["workgroupCountY"].getWithDefault<int64_t>(1) : 1);
            auto wg_z = static_cast<uint32_t>(payload.hasObjectMember("workgroupCountZ")
                ? payload["workgroupCountZ"].getWithDefault<int64_t>(1) : 1);

            if (shader_code.empty()) return choc::value::createBool(false);

            // Create shader module
            wgpu::ShaderSourceWGSL wgsl_desc{};
            wgsl_desc.code = shader_code.c_str();
            wgpu::ShaderModuleDescriptor shader_desc{};
            shader_desc.nextInChain = &wgsl_desc;
            auto shader_module = device_ptr->CreateShaderModule(&shader_desc);
            if (!shader_module) return choc::value::createBool(false);

            // Create compute pipeline
            wgpu::ComputePipelineDescriptor pipe_desc{};
            pipe_desc.compute.module = shader_module;
            pipe_desc.compute.entryPoint = entry_point.c_str();
            auto pipeline = device_ptr->CreateComputePipeline(&pipe_desc);
            if (!pipeline) return choc::value::createBool(false);

            // Create bind groups from serialized data
            std::vector<wgpu::Buffer> gpu_buffers;  // Keep alive until submit
            std::vector<wgpu::BindGroup> bind_groups;

            if (payload.hasObjectMember("bindGroups")) {
                auto bg_data = payload["bindGroups"];
                for (uint32_t bg_idx = 0; bg_idx < bg_data.size(); ++bg_idx) {
                    auto member = bg_data.getObjectMemberAt(bg_idx);
                    auto& entries_val = member.value;

                    std::vector<wgpu::BindGroupEntry> bg_entries;
                    for (uint32_t e = 0; e < entries_val.size(); ++e) {
                        auto entry = entries_val[e];
                        wgpu::BindGroupEntry bge{};
                        bge.binding = static_cast<uint32_t>(
                            entry.hasObjectMember("binding") ? entry["binding"].getWithDefault<int64_t>(0) : 0);

                        if (entry.hasObjectMember("bufferSize")) {
                            auto buf_size = static_cast<uint64_t>(entry["bufferSize"].getWithDefault<int64_t>(0));
                            auto buf_usage = static_cast<uint32_t>(entry["bufferUsage"].getWithDefault<int64_t>(0));

                            wgpu::BufferDescriptor buf_desc{};
                            buf_desc.size = buf_size;
                            buf_desc.usage = static_cast<wgpu::BufferUsage>(buf_usage) |
                                             wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
                            auto gpu_buf = device_ptr->CreateBuffer(&buf_desc);

                            // Decode the base64 payload the JS serializer
                            // (web-compat-gpu-buffered.js) attaches as
                            // `bufferDataBase64` and upload it to the GPU
                            // buffer. Without this every compute dispatch runs
                            // against zeroed buffers regardless of what the JS
                            // shader seeded them with.
                            if (entry.hasObjectMember("bufferDataBase64") && buf_size > 0) {
                                auto b64 = entry["bufferDataBase64"].getWithDefault<std::string>("");
                                if (!b64.empty()) {
#ifdef PULP_BENCHMARK
                                    const double decode_t0 = render::bench::now_us();
                                    auto decoded = runtime::base64_decode(b64);
                                    if (self.bench_counters_) {
                                        self.bench_counters_->base64_decode_total_us.fetch_add(
                                            render::bench::now_us() - decode_t0,
                                            std::memory_order_relaxed);
                                    }
#else
                                    auto decoded = runtime::base64_decode(b64);
#endif
                                    if (decoded) {
                                        const auto& bytes = *decoded;
                                        const uint64_t to_copy = std::min<uint64_t>(
                                            bytes.size(), buf_size);
                                        if (to_copy > 0) {
#ifdef PULP_BENCHMARK
                                            const double t0 = render::bench::now_us();
                                            queue_ptr->WriteBuffer(gpu_buf, 0,
                                                                   bytes.data(),
                                                                   static_cast<size_t>(to_copy));
                                            if (self.bench_counters_) {
                                                self.bench_counters_->gpu_upload_total_us.fetch_add(
                                                    render::bench::now_us() - t0,
                                                    std::memory_order_relaxed);
                                                self.bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                                                    static_cast<double>(to_copy),
                                                    std::memory_order_relaxed);
                                                self.bench_counters_->gpu_buffer_upload_count.fetch_add(
                                                    1.0, std::memory_order_relaxed);
                                                self.bench_counters_->observe_resident_peak(
                                                    static_cast<double>(buf_size));
                                            }
#else
                                            queue_ptr->WriteBuffer(gpu_buf, 0,
                                                                   bytes.data(),
                                                                   static_cast<size_t>(to_copy));
#endif
                                        }
                                    }
                                }
                            }

                            bge.buffer = gpu_buf;
                            bge.size = buf_size;
                            gpu_buffers.push_back(gpu_buf);
                        }
                        bg_entries.push_back(bge);
                    }

                    if (!bg_entries.empty()) {
                        wgpu::BindGroupDescriptor bgd{};
                        bgd.layout = pipeline.GetBindGroupLayout(bg_idx);
                        bgd.entryCount = bg_entries.size();
                        bgd.entries = bg_entries.data();
                        bind_groups.push_back(device_ptr->CreateBindGroup(&bgd));
                    }
                }
            }

            // Encode and dispatch
            wgpu::CommandEncoderDescriptor enc_desc{};
            auto encoder = device_ptr->CreateCommandEncoder(&enc_desc);
            wgpu::ComputePassDescriptor pass_desc{};
            auto pass = encoder.BeginComputePass(&pass_desc);
            pass.SetPipeline(pipeline);
            for (uint32_t i = 0; i < bind_groups.size(); ++i)
                pass.SetBindGroup(i, bind_groups[i]);
            pass.DispatchWorkgroups(wg_x, wg_y, wg_z);
            pass.End();

            auto command_buffer = encoder.Finish();
            queue_ptr->Submit(1, &command_buffer);

            return choc::value::createBool(true);
        } catch (...) {
            return choc::value::createBool(false);
        }
#endif
    });

    // -- DRACO mesh decode (native C++ decoder) --------------------------
    register_bridge_function(api, "__dracoDecodeBuffer", [](choc::javascript::ArgumentList args) {
        (void)args;
        auto result = choc::value::createObject("DracoResult");
        result.addMember("available", choc::value::createBool(
#ifdef PULP_HAS_DRACO
            true
#else
            false
#endif
        ));
        return result;
    });
}

} // namespace pulp::view
