// widget_bridge/gpu_common.cpp - shared native GPU descriptor helpers for WidgetBridge.

#include "gpu_common.hpp"

#if __has_include(<pulp/render/gpu_surface.hpp>)
#include <pulp/render/gpu_surface.hpp>
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 1
#else
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 0
#endif

#include <string_view>

namespace pulp::view::detail {

#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
WidgetBridgeGpuInfo widget_bridge_gpu_info_from_adapter(const render::GpuSurface::AdapterInfo& info) {
    WidgetBridgeGpuInfo result;
    result.available = info.available;
    result.native_bridge = info.native_bridge;
    result.backend = info.backend;
    result.backend_type = info.backend_type;
    result.name = info.name;
    result.preferred_canvas_format = info.preferred_canvas_format;
    result.vendor = info.vendor;
    result.architecture = info.architecture;
    result.description = info.description;
    return result;
}
#endif

choc::value::Value gpu_adapter_info_to_value(const WidgetBridgeGpuInfo& info) {
    auto value = choc::value::createObject("");
    value.addMember("vendor", choc::value::createString(info.vendor));
    value.addMember("architecture", choc::value::createString(info.architecture));
    value.addMember("description", choc::value::createString(info.description));
    value.addMember("backendType", choc::value::createString(info.backend_type));
    return value;
}

choc::value::Value gpu_descriptor_to_value(const WidgetBridgeGpuInfo& info) {
    auto value = choc::value::createObject("");
    value.addMember("available", choc::value::createBool(info.available));
    value.addMember("nativeBridge", choc::value::createBool(info.native_bridge));
    value.addMember("backend", choc::value::createString(info.backend));
    value.addMember("backendType", choc::value::createString(info.backend_type));
    value.addMember("name", choc::value::createString(info.name));
    value.addMember("preferredCanvasFormat", choc::value::createString(info.preferred_canvas_format));
    value.addMember("info", gpu_adapter_info_to_value(info));
    return value;
}

WidgetBridgeGpuInfo widget_bridge_gpu_info(render::GpuSurface* gpu_surface) {
#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
    if (gpu_surface != nullptr) {
        auto info = gpu_surface->adapter_info();
        if (info.available) {
            return widget_bridge_gpu_info_from_adapter(info);
        }
    }
#endif

    WidgetBridgeGpuInfo info{};
    info.available = true;
    info.backend = "Dawn/WebGPU";
    info.preferred_canvas_format = "bgra8unorm";
    return info;
}

std::string gpu_host_object_update_script(const WidgetBridgeGpuInfo& info) {
    const auto string_literal = [] (std::string_view text) {
        return choc::json::toString(choc::value::createString(std::string(text)));
    };
    const auto bool_literal = [] (bool value) {
        return value ? "true" : "false";
    };

    std::string script;
    script.reserve(512);
    script += "if (typeof navigatorGPU !== 'undefined' && navigatorGPU) {";
    script += "navigatorGPU.backend = " + string_literal(info.backend) + ";";
    script += "navigatorGPU.backendType = " + string_literal(info.backend_type) + ";";
    script += "navigatorGPU.available = " + std::string(bool_literal(info.available)) + ";";
    script += "navigatorGPU.nativeBridge = " + std::string(bool_literal(info.native_bridge)) + ";";
    script += "}";
    return script;
}

} // namespace pulp::view::detail
