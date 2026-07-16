#pragma once

#include <pulp/view/widget_bridge.hpp>

#ifdef PULP_HAS_SKIA
#include "webgpu/webgpu_cpp.h"
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view::detail {

struct WidgetBridgeGpuInfo {
    bool available = false;
    bool native_bridge = false;
    std::string backend;
    std::string backend_type;
    std::string name;
    std::string preferred_canvas_format;
    std::string vendor;
    std::string architecture;
    std::string description;
};

choc::value::Value gpu_adapter_info_to_value(const WidgetBridgeGpuInfo& info);
choc::value::Value gpu_descriptor_to_value(const WidgetBridgeGpuInfo& info);
WidgetBridgeGpuInfo widget_bridge_gpu_info(render::GpuSurface* gpu_surface);
std::string gpu_host_object_update_script(const WidgetBridgeGpuInfo& info);

} // namespace pulp::view::detail

namespace pulp::view {

struct WidgetBridge::NativeGpuBridgeState {
    struct CanvasContextState {
        uint32_t width = 1;
        uint32_t height = 1;
        std::string format = "bgra8unorm";
        uint32_t usage = 0;
        std::string alpha_mode = "opaque";
#ifdef PULP_HAS_SKIA
        wgpu::Texture texture;
#endif
        bool configured = false;
    };

    struct TextureState {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth_or_array_layers = 1;
        std::string format = "bgra8unorm";
        uint32_t usage = 0;
        uint32_t mip_level_count = 1;
        uint32_t sample_count = 1;
#ifdef PULP_HAS_SKIA
        wgpu::Texture texture;
#endif
        bool configured = false;
    };

    std::unordered_map<std::string, CanvasContextState> canvases;
    std::unordered_map<std::string, TextureState> textures;
    uint64_t next_texture_id = 1;
};

} // namespace pulp::view
