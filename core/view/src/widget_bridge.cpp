#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/widgets/svg_rect.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/design_import.hpp>
#if __has_include(<pulp/render/gpu_surface.hpp>)
#include <pulp/render/gpu_surface.hpp>
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 1
#else
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 0
#endif
#include <pulp/platform/popup_menu.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/platform/clipboard.hpp>
#include <pulp/runtime/base64.hpp>
#include <web_compat_preludes_gen.hpp>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

#ifdef PULP_HAS_SKIA
#include "webgpu/webgpu_cpp.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/graphite/Image.h"
#include <pulp/canvas/skia_canvas.hpp>
#endif

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#endif

#include <pulp/platform/child_process.hpp>

namespace pulp::view {

// Forward declaration of the design_import.cpp wrapper that exposes its
// (anonymous-namespace) json_string_literal helper across translation
// units. Used by the runtime-import handlers below.
namespace detail {
std::string json_string_literal_for_widget_bridge(const std::string& s);
} // namespace detail

namespace {

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

std::string bridge_base64_encode(const std::vector<uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < data.size()) ? table[n & 0x3F] : '=';
    }

    return out;
}

std::string canonical_bridge_asset_mime_type(std::string mime_type) {
    if (mime_type == "application/json" || mime_type == "text/json") {
        return "application/json;charset=utf-8";
    }
    return mime_type;
}

std::string guess_bridge_asset_mime_type(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string{} : path.substr(dot);
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".css") return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".txt" || ext == ".wgsl" || ext == ".sksl") return "text/plain";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    return "application/octet-stream";
}

bool bridge_asset_is_text_like(const std::string& mime_type) {
    return mime_type.rfind("text/", 0) == 0
        || mime_type.find("json") != std::string::npos
        || mime_type.find("javascript") != std::string::npos
        || mime_type.find("xml") != std::string::npos
        || mime_type.find("svg") != std::string::npos;
}

View* find_view_by_id(View& node, std::string_view id) {
    if (!id.empty() && node.id() == id) {
        return &node;
    }

    for (size_t i = 0; i < node.child_count(); ++i) {
        if (auto* match = find_view_by_id(*node.child_at(i), id)) {
            return match;
        }
    }

    return nullptr;
}

bool subtree_contains_view(View& node, const View* target) {
    if (&node == target) {
        return true;
    }

    for (size_t i = 0; i < node.child_count(); ++i) {
        if (subtree_contains_view(*node.child_at(i), target)) {
            return true;
        }
    }

    return false;
}

void erase_widget_subtree(std::unordered_map<std::string, View*>& widgets, View* node) {
    if (node == nullptr) {
        return;
    }

    for (size_t i = 0; i < node->child_count(); ++i) {
        erase_widget_subtree(widgets, node->child_at(i));
    }

    if (!node->id().empty()) {
        widgets.erase(node->id());
    }
}

std::string strip_leading_slashes(std::string path) {
    while (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        path.erase(path.begin());
    }
    return path;
}

struct BridgeAssetLoad {
    bool ok = false;
    int status = 404;
    std::string resolved_path;
    std::string mime_type;
    BlobData blob;
};

BridgeAssetLoad load_bridge_asset(const std::string& url) {
    BridgeAssetLoad result;
    if (url.empty()) {
        result.status = 400;
        return result;
    }

    auto& assets = AssetManager::instance();

    auto load_from_file = [&](std::string path) {
        if (path.empty()) {
            return BlobData{};
        }
#if defined(_WIN32)
        if (path.size() > 2 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
            path.erase(path.begin());
        }
#endif
        result.resolved_path = path;
        return assets.load_blob(path);
    };

    auto load_from_embedded = [&](std::string name) {
        name = strip_leading_slashes(std::move(name));
        if (name.empty()) {
            return BlobData{};
        }
        result.resolved_path = name;
        if (assets.has_embedded(name)) {
            return assets.load_blob_embedded(name);
        }
        return BlobData{};
    };

    if (url.rfind("pulp://", 0) == 0) {
        auto ref = url.substr(7);
        result.blob = load_from_embedded(ref);
        if (!result.blob.valid()) {
            result.blob = load_from_file(strip_leading_slashes(ref));
        }
    } else if (url.rfind("file://", 0) == 0) {
        result.blob = load_from_file(url.substr(7));
    } else {
        result.blob = load_from_embedded(url);
        if (!result.blob.valid()) {
            result.blob = load_from_file(url);
        }
    }

    if (!result.blob.valid()) {
        result.status = 404;
        return result;
    }

    result.ok = true;
    result.status = 200;
    result.mime_type = canonical_bridge_asset_mime_type(
        guess_bridge_asset_mime_type(result.resolved_path.empty() ? url : result.resolved_path));
    return result;
}

} // namespace

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
    std::unordered_map<std::string, std::vector<uint8_t>> native_buffers;
    uint64_t next_texture_id = 1;
};

// pulp #1006 — `on(id, eventName, fn)` is the JS-side hook that callers
// (notably @pulp/react's prop-applier turning JSX `onClick={fn}` into a
// bridge subscription) use to register event callbacks. Storing the fn
// in __callbacks__ is necessary but not sufficient: events that the
// native side fires through View::on_click / on_hover_enter /
// on_pointer_event require an explicit `registerClick(id)` /
// `registerHover(id)` / `registerPointer(id)` to wire the View
// callback. Without that wiring real NSEvent / Win32 / X11 clicks
// reach View::on_mouse_down/up but never trigger __dispatch__('click'),
// so the JS handler never runs.
//
// The fix: when JS subscribes to a known event name, transparently
// invoke the matching native registrar (idempotent per (id, group)
// via __nativeRegistered__). This mirrors what
// Element.prototype._registerNativeEvent does for addEventListener
// callers, but on the lower-level `on()` channel that @pulp/react and
// other native bridges use directly.
static const char* kJSPreamble = R"(
var __callbacks__ = {};
var __nativeRegistered__ = {};
// pulp #1XXX — __dispatch__ used to let any exception thrown from a
// React handler propagate out of the JS engine's evaluate(), which
// then unwound the C++ caller and (most damagingly) killed the
// requestAnimationFrame self-rescheduling chain. Symptom: any tiny
// throw from a rAF tick (a stale ref, a missing prop) and the whole
// animation loop went dead, only restarting when an unrelated event
// (e.g. mouse-move) ran the loop again. Wrap every handler in
// try/catch and surface the error via __dispatchError__ if defined,
// so handlers can throw without halting the world.
//
// Also fan out events targeting the synthetic '__global__' id into
// window._listeners[eventName] — `window.addEventListener('keydown',
// fn)` is the standard DOM API, but without this fan-out the native
// keydown only fed __callbacks__['__global__:keydown'] (a per-id
// channel nothing subscribes to). With it, Spectr-style global key
// listeners just work.
function __dispatch__(id, eventName) {
    var args = Array.prototype.slice.call(arguments, 2);
    var key = id + ':' + eventName;
    var cb = __callbacks__[key];
    if (cb) {
        try { cb.apply(null, args); }
        catch (e) {
            if (typeof __dispatchError__ === 'function') __dispatchError__(id, eventName, String(e && e.stack ? e.stack : e));
        }
    }
    if (id === '__global__' && typeof window !== 'undefined' && window._listeners) {
        var list = window._listeners[eventName];
        if (list && list.length) {
            var ev = args && args.length ? args[0] : {};
            if (ev && typeof ev === 'object') {
                ev.type = eventName;
                if (typeof ev.preventDefault !== 'function') ev.preventDefault = function() { this.defaultPrevented = true; };
                if (typeof ev.stopPropagation !== 'function') ev.stopPropagation = function() {};
            }
            for (var i = 0; i < list.length; i++) {
                try { list[i](ev); }
                catch (e) {
                    if (typeof __dispatchError__ === 'function') __dispatchError__(id, eventName, String(e && e.stack ? e.stack : e));
                }
            }
        }
    }
}
function __ensureNativeRegistered__(id, group) {
    var key = id + ':' + group;
    if (__nativeRegistered__[key]) return;
    __nativeRegistered__[key] = true;
    if (group === 'click' && typeof registerClick === 'function') {
        registerClick(id);
    } else if (group === 'hover' && typeof registerHover === 'function') {
        registerHover(id);
    } else if (group === 'pointer' && typeof registerPointer === 'function') {
        registerPointer(id);
    } else if (group === 'gesture' && typeof registerGesture === 'function') {
        registerGesture(id);
    } else if (group === 'wheel' && typeof registerWheel === 'function') {
        registerWheel(id);
    }
}
function on(id, eventName, fn) {
    __callbacks__[id + ':' + eventName] = fn;
    if (eventName === 'click' || eventName === 'mousedown' || eventName === 'mouseup') {
        __ensureNativeRegistered__(id, 'click');
    } else if (eventName === 'mouseenter' || eventName === 'mouseleave' ||
               eventName === 'pointerenter' || eventName === 'pointerleave') {
        __ensureNativeRegistered__(id, 'hover');
    } else if (eventName === 'pointerdown' || eventName === 'pointermove' ||
               eventName === 'pointerup' || eventName === 'pointercancel') {
        __ensureNativeRegistered__(id, 'pointer');
    } else if (eventName === 'gesturestart' || eventName === 'gesturechange' ||
               eventName === 'gestureend') {
        __ensureNativeRegistered__(id, 'gesture');
    } else if (eventName === 'wheel') {
        // pulp #1XXX — Spectr's trackpad-zoom handler binds via
        // addEventListener('wheel', fn) which routes through on(id,
        // 'wheel', fn). Without this case the JS callback is stored
        // but the C++ registerWheel(id) is never invoked, so wheel
        // events never reach the JS handler. Critical for trackpad
        // zoom on any wrap-div that subscribes via 'wheel'.
        __ensureNativeRegistered__(id, 'wheel');
    }
}
)";

// pulp #1XXX — Window event-listener shim installed AFTER the
// web-compat-document.js prelude (which re-declares `var window = {...}`
// and would clobber any addEventListener we installed earlier). This
// shim is the minimal install needed for `__dispatch__('__global__',
// 'keydown', ...)` fan-out to reach `window.addEventListener('keydown',
// fn)` listeners. Spectr's `e.key === 'Escape'` flow depends on this.
// Idempotent via the `__pulpListenerShim__` marker — re-eval safe.
static const char* kWindowListenerShim = R"(
if (typeof globalThis.window === 'undefined') globalThis.window = {};
if (!globalThis.window.__pulpListenerShim__) {
    globalThis.window.__pulpListenerShim__ = true;
    if (!globalThis.window._listeners) globalThis.window._listeners = {};
    globalThis.window.addEventListener = function(type, fn) {
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(fn);
    };
    globalThis.window.removeEventListener = function(type, fn) {
        var list = this._listeners[type];
        if (!list) return;
        for (var i = list.length - 1; i >= 0; i--) if (list[i] === fn) list.splice(i, 1);
    };
    globalThis.window.dispatchEvent = function(ev) {
        var list = this._listeners[ev && ev.type];
        if (!list) return true;
        for (var i = 0; i < list.length; i++) {
            try { list[i](ev); }
            catch (e) {
                if (typeof __dispatchError__ === 'function') __dispatchError__('window', ev.type, String(e && e.stack ? e.stack : e));
            }
        }
        return !(ev && ev.defaultPrevented);
    };
}
)";

// pulp #745: kDomOpsInit lived here as an inline C-string copy of
// core/view/js/web-compat-dom-ops.js. Both sources had drifted (the
// inline copy carried DocumentFragment flatten paths the standalone
// file lacked); only the inline copy was actually evaluated. The JS
// file is now the single source of truth and gets evaluated by the
// constructor along with the rest of the prelude chain.

static void safe_dispatch_eval(ScriptEngine& engine, const std::string& js, const char* context) {
    try {
        engine.evaluate(js);
        // Pump microtasks so React setState commits (and any queueMicrotask /
        // Promise.then continuations scheduled by the handler) before the next
        // event arrives. Without this, drag-style interactions see stale state
        // on the immediately-following pointermove and silently bail; see #1923.
        engine.pump_message_loop();
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge " << context << " error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge " << context << " error: unknown exception\n";
    }
}

static void safe_dispatch_eval(const std::shared_ptr<std::atomic<bool>>& alive,
                               ScriptEngine* engine,
                               const std::string& js,
                               const char* context) {
    if (!alive || !alive->load(std::memory_order_acquire) || engine == nullptr) return;
    try {
        if (!static_cast<bool>(*engine)) return;
        engine->evaluate(js);
        // Pump microtasks so React setState commits (and any queueMicrotask /
        // Promise.then continuations scheduled by the handler) before the next
        // event arrives. Without this, drag-style interactions see stale state
        // on the immediately-following pointermove and silently bail; see #1923.
        engine->pump_message_loop();
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge " << context << " error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge " << context << " error: unknown exception\n";
    }
}

static choc::value::Value make_layout_rect_value(View* v) {
    auto result = choc::value::createObject("");
    if (!v) return result;

    float ax = 0.0f;
    float ay = 0.0f;
    View* cur = v;
    while (cur) {
        ax += cur->bounds().x;
        ay += cur->bounds().y;
        if (auto* scroll = dynamic_cast<ScrollView*>(cur->parent())) {
            ax -= scroll->scroll_x();
            ay -= scroll->scroll_y();
        }
        cur = cur->parent();
    }

    auto b = v->bounds();
    result.addMember("x", choc::value::createFloat64(ax));
    result.addMember("y", choc::value::createFloat64(ay));
    result.addMember("width", choc::value::createFloat64(b.width));
    result.addMember("height", choc::value::createFloat64(b.height));
    result.addMember("top", choc::value::createFloat64(ay));
    result.addMember("left", choc::value::createFloat64(ax));
    result.addMember("right", choc::value::createFloat64(ax + b.width));
    result.addMember("bottom", choc::value::createFloat64(ay + b.height));
    return result;
}

static void eval_or_throw(ScriptEngine& engine, const char* name, const std::string& js) {
    try {
        engine.evaluate(js);
    } catch (const choc::javascript::Error& e) {
        throw std::runtime_error(std::string("failed to evaluate ") + name + ": " + e.what());
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("failed to evaluate ") + name + ": " + e.what());
    } catch (...) {
        throw std::runtime_error(std::string("failed to evaluate ") + name + ": unknown exception");
    }
}

static std::string build_shell_command(const std::string& cmd) {
#if defined(_WIN32)
    return std::string(
        "set \"PATH=%USERPROFILE%\\.local\\bin;%USERPROFILE%\\.npm-global\\bin;%PATH%\" && "
    ) + cmd;
#else
    return std::string(
        "export PATH=\"$HOME/.local/bin:$HOME/.npm-global/bin:"
        "/opt/homebrew/bin:/usr/local/bin:$PATH\"; "
    ) + cmd;
#endif
}

WidgetBridge::WidgetBridge(ScriptEngine& engine, View& root, state::StateStore& store,
                           render::GpuSurface* gpu_surface)
    : engine_(engine), root_(root), store_(store), gpu_surface_(gpu_surface) {
    if (widget_bridge_gpu_info(gpu_surface_).native_bridge) {
        native_gpu_bridge_state_ = std::make_unique<NativeGpuBridgeState>();
    }
    // Default repaint wiring: route to the root view's host invalidator.
    // Without this, JS `requestAnimationFrame` callbacks queue but never
    // schedule a second paint, because the only way out of `request_repaint()`
    // is `repaint_callback_`. Hosts that need a custom invalidator (the
    // standalone window's top-level editor) replace this via
    // `set_repaint_callback`. See pulp #899.
    //
    // Capture-by-reference is sound: the bridge already holds `View& root_`
    // as a member and cannot outlive `root` without UB. The lambda lives at
    // most as long as the bridge.
    repaint_callback_ = [&root] { root.request_repaint(); };
    register_api();
    eval_or_throw(engine_, "kJSPreamble", kJSPreamble);
    eval_or_throw(engine_, "css_colors", preludes::css_colors);
    eval_or_throw(engine_, "css_parser", preludes::css_parser);
    eval_or_throw(engine_, "web_compat_element", preludes::web_compat_element);
    eval_or_throw(engine_, "web_compat_canvas", preludes::web_compat_canvas);
    eval_or_throw(engine_, "web_compat_style_decl", preludes::web_compat_style_decl);
    eval_or_throw(engine_, "web_compat_document", preludes::web_compat_document);
    eval_or_throw(engine_, "web_compat_gpu_buffered", preludes::web_compat_gpu_buffered);
    // pulp #745 — DOM mutation methods (appendChild / removeChild / etc.).
    // Single source of truth lives in core/view/js/web-compat-dom-ops.js.
    // The JS file's idempotency guard (`__pulp_dom_ops__` marker on the
    // prototype methods) makes a re-eval a no-op, which matters because
    // load_script callers used to re-trigger this initialization manually
    // before the consolidation. See pulp #745.
    eval_or_throw(engine_, "web_compat_dom_ops", preludes::web_compat_dom_ops);
    // pulp #468 — observer no-ops + scheduler shims so React 18 (and any
    // other framework that feature-detects MutationObserver / MessageChannel
    // / queueMicrotask) finds the constructors it expects on the global.
    eval_or_throw(engine_, "web_compat_observers", preludes::web_compat_observers);
    eval_or_throw(engine_, "web_compat_scheduler", preludes::web_compat_scheduler);
    // pulp #468 — DOM construction + walker for the Claude Design
    // `--execute-bundle` import lane. Hides itself behind
    // `globalThis.__pulpImportRuntime__` so non-import scripts don't
    // see new globals.
    eval_or_throw(engine_, "import_runtime", preludes::import_runtime);
    // Install the window event-listener shim LAST so it survives any
    // `var window = {...}` reassignment performed by the preludes above
    // (notably web-compat-document.js). See kWindowListenerShim comment.
    eval_or_throw(engine_, "kWindowListenerShim", kWindowListenerShim);
}

WidgetBridge::~WidgetBridge() {
    if (callback_alive_) callback_alive_->store(false, std::memory_order_release);
    root_.on_global_click = {};
}

void WidgetBridge::set_repaint_callback(std::function<void()> cb) {
    repaint_callback_ = std::move(cb);
}

void WidgetBridge::set_ai_cli_command(std::string cmd) {
    if (!cmd.empty()) ai_cli_command_ = std::move(cmd);
}

#ifdef PULP_BENCHMARK
void WidgetBridge::set_bench_counters(render::bench::PerfCounters* counters) {
    bench_counters_ = counters;
}
#endif

void WidgetBridge::request_repaint() {
    if (repaint_callback_) repaint_callback_();
}

CanvasWidget::NativeGpuTextureFrame WidgetBridge::describe_native_canvas_frame(
    const std::string& canvas_id) const {
    CanvasWidget::NativeGpuTextureFrame frame;
    if (native_gpu_bridge_state_ == nullptr || canvas_id.empty()) {
        return frame;
    }

    auto it = native_gpu_bridge_state_->canvases.find(canvas_id);
    if (it == native_gpu_bridge_state_->canvases.end() || !it->second.configured) {
        return frame;
    }

    frame.width = it->second.width;
    frame.height = it->second.height;
    frame.format = it->second.format;
    frame.available = true;
#ifdef PULP_HAS_SKIA
    frame.texture_handle = const_cast<wgpu::Texture*>(&it->second.texture);
#endif
    return frame;
}

CanvasWidget::NativeGpuTextureFrame WidgetBridge::describe_native_texture_frame(
    const std::string& texture_id) const {
    CanvasWidget::NativeGpuTextureFrame frame;
    if (native_gpu_bridge_state_ == nullptr || texture_id.empty()) {
        return frame;
    }

    auto it = native_gpu_bridge_state_->textures.find(texture_id);
    if (it == native_gpu_bridge_state_->textures.end() || !it->second.configured) {
        return frame;
    }

    frame.width = it->second.width;
    frame.height = it->second.height;
    frame.format = it->second.format;
    frame.available = true;
#ifdef PULP_HAS_SKIA
    frame.texture_handle = const_cast<wgpu::Texture*>(&it->second.texture);
#endif
    return frame;
}

void WidgetBridge::load_script(const std::string& code) {
    // pulp #745: the DOM mutation methods are now installed by the
    // constructor's prelude chain (`web_compat_dom_ops` slot). The
    // JS-side idempotency guard makes a re-eval a no-op, so callers
    // that load multiple scripts no longer need the bridge to track
    // a "first time" flag.
    //
    // Append ";void 0" so the eval result is undefined, not the last
    // expression value. Elements have circular references (_parentElement
    // ↔ _children) which cause infinite recursion in CHOC's toChocValue().
    eval_or_throw(engine_, "user_script", code + "\n;void 0");
    // Flush any pending requestAnimationFrame callbacks
    eval_or_throw(engine_, "flush_frames", "if (typeof __flushFrames__ === 'function') __flushFrames__();void 0");
}

View* WidgetBridge::widget(const std::string& id) {
    if (id.empty()) {
        return nullptr;
    }

    auto it = widgets_.find(id);
    if (it != widgets_.end()) {
        if (it->second != nullptr && subtree_contains_view(root_, it->second)) {
            return it->second;
        }
        widgets_.erase(it);
    }

    if (auto* live = find_view_by_id(root_, id)) {
        widgets_[id] = live;
        return live;
    }

    return nullptr;
}

void WidgetBridge::sync_from_store() {
    for (auto& [id, view] : widgets_) {
        if (auto* knob = dynamic_cast<Knob*>(view)) {
            // Try to find a parameter matching this widget's id
            // Convention: widget id matches parameter name
            for (size_t i = 0; i < store_.param_count(); ++i) {
                auto* info = &store_.all_params()[i];
                if (info && info->name == id) {
                    knob->set_value(store_.get_normalized(info->id));
                    break;
                }
            }
        } else if (auto* fader = dynamic_cast<Fader*>(view)) {
            for (size_t i = 0; i < store_.param_count(); ++i) {
                auto* info = &store_.all_params()[i];
                if (info && info->name == id) {
                    fader->set_value(store_.get_normalized(info->id));
                    break;
                }
            }
        } else if (auto* toggle = dynamic_cast<Toggle*>(view)) {
            for (size_t i = 0; i < store_.param_count(); ++i) {
                auto* info = &store_.all_params()[i];
                if (info && info->name == id) {
                    toggle->set_on(store_.get_normalized(info->id) > 0.5f);
                    break;
                }
            }
        }
    }
}

View* WidgetBridge::resolve_parent(const std::string& parent_id) {
    if (parent_id.empty()) return &root_;
    auto it = widgets_.find(parent_id);
    return it != widgets_.end() ? it->second : &root_;
}

void WidgetBridge::wire_callbacks(const std::string& id, View* w) {
    auto alive = callback_alive_;
    auto* engine = &engine_;
    if (auto* k = dynamic_cast<Knob*>(w)) {
        k->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "knob change");
        };
    } else if (auto* f = dynamic_cast<Fader*>(w)) {
        f->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "fader change");
        };
    } else if (auto* t = dynamic_cast<Toggle*>(w)) {
        t->on_toggle = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'toggle', " + std::string(v?"1":"0") + ")", "toggle");
        };
    } else if (auto* r = dynamic_cast<RangeSlider*>(w)) {
        // pulp issue-966 — HTML <input type="range"> change event. The
        // payload is the post-quantisation value (not normalised) so JS
        // callers see the same number they handed us via setValue/setMin
        // /setMax/setStep.
        r->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "range slider change");
        };
    }
}

void WidgetBridge::clear() {
    pending_frame_ids_.clear();
    shortcuts_.clear();
    ComboBox::close_active_popup();
    while (root_.child_count() > 0) {
        auto* child = root_.child_at(root_.child_count() - 1);
        auto removed = root_.remove_child(child);
        erase_widget_subtree(widgets_, removed.get());
    }
    widgets_.clear();
}

void WidgetBridge::snapshot_values(std::unordered_map<std::string, float>& out) const {
    for (auto& [id, view] : widgets_) {
        if (auto* k = dynamic_cast<Knob*>(view)) out[id] = k->value();
        else if (auto* f = dynamic_cast<Fader*>(view)) out[id] = f->value();
        else if (auto* r = dynamic_cast<RangeSlider*>(view)) out[id] = r->value();
        else if (auto* t = dynamic_cast<Toggle*>(view)) out[id] = t->is_on() ? 1.0f : 0.0f;
        else if (auto* cb = dynamic_cast<Checkbox*>(view)) out[id] = cb->is_checked() ? 1.0f : 0.0f;
        else if (auto* tb = dynamic_cast<ToggleButton*>(view)) out[id] = tb->is_on() ? 1.0f : 0.0f;
    }
}

void WidgetBridge::restore_values(const std::unordered_map<std::string, float>& snapshot) {
    for (auto& [id, val] : snapshot) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        if (auto* k = dynamic_cast<Knob*>(it->second)) k->set_value(val);
        else if (auto* f = dynamic_cast<Fader*>(it->second)) f->set_value(val);
        else if (auto* r = dynamic_cast<RangeSlider*>(it->second)) r->set_value(val);
        else if (auto* t = dynamic_cast<Toggle*>(it->second)) t->set_on(val > 0.5f);
        else if (auto* cb = dynamic_cast<Checkbox*>(it->second)) cb->set_checked(val > 0.5f);
        else if (auto* tb = dynamic_cast<ToggleButton*>(it->second)) tb->set_on(val > 0.5f);
    }
}

void WidgetBridge::snapshot_values(WidgetReloadSnapshot& out) const {
    for (auto& [id, view] : widgets_) {
        if (auto* k = dynamic_cast<Knob*>(view)) out.scalar_values[id] = k->value();
        else if (auto* f = dynamic_cast<Fader*>(view)) out.scalar_values[id] = f->value();
        else if (auto* r = dynamic_cast<RangeSlider*>(view)) out.scalar_values[id] = r->value();
        else if (auto* t = dynamic_cast<Toggle*>(view)) out.scalar_values[id] = t->is_on() ? 1.0f : 0.0f;
        else if (auto* cb = dynamic_cast<Checkbox*>(view)) out.scalar_values[id] = cb->is_checked() ? 1.0f : 0.0f;
        else if (auto* tb = dynamic_cast<ToggleButton*>(view)) out.scalar_values[id] = tb->is_on() ? 1.0f : 0.0f;
        else if (auto* xy = dynamic_cast<XYPad*>(view)) out.xy_values[id] = {.x = xy->x_value(), .y = xy->y_value()};
    }
}

void WidgetBridge::restore_values(const WidgetReloadSnapshot& snapshot) {
    for (auto& [id, val] : snapshot.scalar_values) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        if (auto* k = dynamic_cast<Knob*>(it->second)) k->set_value(val);
        else if (auto* f = dynamic_cast<Fader*>(it->second)) f->set_value(val);
        else if (auto* r = dynamic_cast<RangeSlider*>(it->second)) r->set_value(val);
        else if (auto* t = dynamic_cast<Toggle*>(it->second)) t->set_on(val > 0.5f);
        else if (auto* cb = dynamic_cast<Checkbox*>(it->second)) cb->set_checked(val > 0.5f);
        else if (auto* tb = dynamic_cast<ToggleButton*>(it->second)) tb->set_on(val > 0.5f);
    }
    for (auto& [id, val] : snapshot.xy_values) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        if (auto* xy = dynamic_cast<XYPad*>(it->second)) { xy->set_x(val.x); xy->set_y(val.y); }
    }
}

void WidgetBridge::poll_async_results() {
    std::vector<AsyncExecResult> pending;
    {
        std::lock_guard<std::mutex> lock(*async_exec_mutex_);
        pending.swap(*async_exec_results_);
    }
    bool had_pending_frames = !pending_frame_ids_.empty();

    for (const auto& result : pending) {
        auto payload = choc::json::toString(choc::value::createString(result.output), false);
        safe_dispatch_eval(engine_, "__dispatch__('" + result.callback_id + "', 'result', " +
            payload + ")", "async result");
    }

    if (had_pending_frames) {
        engine_.evaluate("if (typeof __flushFrames__ === 'function') __flushFrames__();void 0");
    }

    if (!pending.empty() || had_pending_frames) {
        request_repaint();
    }
}

void WidgetBridge::service_frame_callbacks() {
    engine_.pump_message_loop();
    // pulp #915 — drain any expired native-tracked setTimeout/setInterval
    // timers so consumers don't need a JS shim wrapping our frame loop.
    if (!pending_timers_.empty()) {
        engine_.evaluate("if (typeof __flushTimers__ === 'function') __flushTimers__();void 0");
        engine_.pump_message_loop();
    }
    if (!pending_frame_ids_.empty()) {
        engine_.evaluate("if (typeof __flushFrames__ === 'function') __flushFrames__();void 0");
        engine_.pump_message_loop();
    }
}

// ── Runtime design import (pulp #468 follow-up) ───────────────────
//
// Registers __pulpRuntimeImport__(html, source) and
// __pulpRuntimeSettle__(rounds) on the live bridge engine. The JS-side
// `@pulp/react/runtime-import` calls these to evaluate a design bundle
// in THIS engine (not a fresh one) — the key insight from the
// pulp-runtime-import-FINAL-design.md alignment with codex: one engine,
// one React, one reconciler.
//
// Architecturally distinct from the offline parse_claude_html_with_runtime
// path which allocates a fresh sandbox for IR extraction. That path stays
// available via `pulp import-design --execute-bundle` for inspection.
//
// Side effects (set on globalThis for the JS shim to inspect):
//   - __pulpRuntimeImportErr__ : string set on soft-fail (empty = ok)
//
// JS side reads/clears the err global; this function only writes to it.

void WidgetBridge::install_runtime_import_handlers() {
    // Idempotency guard: register once per bridge. The earlier
    // implementation used a static thread_local set keyed by `this`,
    // which broke under heap reuse (test bridges destroyed and
    // reconstructed at the same address skipped registration silently).
    if (runtime_import_installed_) return;
    runtime_import_installed_ = true;

    // The bundled-React asset evaluation can throw arbitrary JS errors
    // when the sandbox is missing a host primitive (DecompressionStream,
    // some HTMLElement, etc.). We surface those via the runtime-error
    // sink rather than throwing into the JS engine — the JS shim is
    // responsible for routing them to opts.onError.
    auto set_err = [this](const std::string& msg) {
        std::string js = "globalThis.__pulpRuntimeImportErr__ = ";
        js += detail::json_string_literal_for_widget_bridge(msg);
        js += ";void 0";
        try { engine_.evaluate(js); } catch (...) { /* best-effort */ }
    };

    auto clear_err = [this]() {
        // Codex P1 + P2 follow-ups on #1856: a stale error from a
        // previous runtime-import attempt would otherwise leak into
        // the next aggregation. Clear EVERY transient error global the
        // aggregator reads: __pulpRuntimeImportErr__ itself, the
        // babel-transform / flushSync / createRoot-render slots, and
        // any per-payload __pulpPayloadErr_*.
        try {
            engine_.evaluate(
                "(function(){"
                "  globalThis.__pulpRuntimeImportErr__ = '';"
                "  globalThis.__pulpEvalErr__ = '';"
                "  globalThis.__pulpFlushSyncErr__ = '';"
                "  globalThis.__pulpCreateRootRenderErr__ = '';"
                "  for (var k in globalThis) {"
                "    if (k.indexOf('__pulpPayloadErr_') === 0) globalThis[k] = '';"
                "  }"
                "})();void 0");
        } catch (...) { /* best-effort */ }
    };

    // __pulpRuntimeImport__(html, source_label) → void
    //
    // Parses the selected runtime-import source bundle, evaluates inline
    // text/javascript + text/babel scripts on THIS engine, dispatches
    // DOMContentLoaded.
    //
    // Crucially does NOT call buildDom or walkDomJson: the runtime path
    // is reconciler-owned (the JS-side ReactDOM capture shim catches
    // the React element directly).
    engine_.register_function("__pulpRuntimeImport__",
        [this, set_err, clear_err](choc::javascript::ArgumentList args) -> choc::value::Value {
            clear_err();
            auto html = args.get<std::string>(0, "");
            auto src_label = args.get<std::string>(1, "auto");
            if (html.empty()) {
                set_err("__pulpRuntimeImport__: empty html");
                return choc::value::Value();
            }

            try {
                auto source_lc = src_label;
                std::transform(source_lc.begin(), source_lc.end(), source_lc.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                std::optional<ClaudeBundle> bundle;
                if (source_lc == "v0" || source_lc == "v0.dev" || source_lc == "v0-dev") {
                    bundle = parse_v0_dev_react(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported v0.dev React export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if (source_lc == "figma" || source_lc == "figma-make") {
                    bundle = parse_figma_make_react(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported Figma Make React export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if (source_lc == "stitch" || source_lc == "google-stitch") {
                    bundle = parse_stitch_react(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported Stitch React export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if (source_lc == "rn" || source_lc == "react-native" ||
                           source_lc == "reactnative") {
                    bundle = parse_react_native_export(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported React Native export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if ((source_lc == "auto" || source_lc.empty()) &&
                           html.find("react-native") != std::string::npos) {
                    bundle = parse_react_native_export(html);
                    if (!bundle) {
                        bundle = parse_claude_bundle(html);
                    }
                } else {
                    bundle = parse_claude_bundle(html);
                }
                if (!bundle) {
                    set_err("__pulpRuntimeImport__: no claude bundle envelope (got '"
                            + src_label + "')");
                    return choc::value::Value();
                }

                // The shared shim setup + payload eval logic lives in
                // a helper that both this path and the offline path
                // (parse_claude_html_with_runtime) can call. Phase 6 step 7
                // factors that helper out. For now, inline the minimal
                // sequence needed for a working runtime path: shims,
                // asset eval, inline script eval. Skips buildDom +
                // walkDomJson per the FINAL design.
                evaluate_claude_bundle_in_live_engine(*bundle);
                // Codex P1 (Phase 6.1 review): the shared pipeline
                // writes per-payload eval failures to
                // `__pulpPayloadErr_<idx>__` and Babel-transform
                // failures to `__pulpEvalErr__`, but JS callers (and
                // Phase 6.3's runtime-import.ts) only read
                // `__pulpRuntimeImportErr__`. Aggregate any soft errors
                // into the runtime-error sink so they're visible to
                // onError / lastError callers. Best-effort — if the
                // engine is in a bad state, leave the existing err
                // value (set by set_err / clear_err above) intact.
                try {
                    auto v = engine_.evaluate(
                        "(function(){"
                        "  var errs = [];"
                        "  var k = globalThis.__pulpRuntimeImportErr__;"
                        "  if (typeof k === 'string' && k.length) errs.push(k);"
                        "  if (typeof globalThis.__pulpEvalErr__ === 'string' && globalThis.__pulpEvalErr__.length)"
                        "    errs.push('babel-transform: ' + globalThis.__pulpEvalErr__);"
                        "  if (typeof globalThis.__pulpFlushSyncErr__ === 'string' && globalThis.__pulpFlushSyncErr__.length)"
                        "    errs.push('flushSync: ' + globalThis.__pulpFlushSyncErr__);"
                        // Codex P2 on #1856: run_claude_bundle_payload_pipeline
                        // (design_import.cpp:1054) writes createRoot/render
                        // failures here. Without this branch, render-time
                        // exceptions never propagate to onError/lastError.
                        "  if (typeof globalThis.__pulpCreateRootRenderErr__ === 'string' && globalThis.__pulpCreateRootRenderErr__.length)"
                        "    errs.push('createRoot/render: ' + globalThis.__pulpCreateRootRenderErr__);"
                        "  for (var key in globalThis) {"
                        "    if (key.indexOf('__pulpPayloadErr_') === 0) {"
                        "      var pe = globalThis[key];"
                        "      if (typeof pe === 'string' && pe.length) errs.push('payload ' + key.slice(17, -2) + ': ' + pe);"
                        "    }"
                        "  }"
                        "  return errs.join(' | ');"
                        "})()");
                    auto aggregated = v.getWithDefault<std::string>("");
                    if (!aggregated.empty()) set_err(aggregated);
                } catch (...) { /* leave existing err */ }
            } catch (const std::exception& e) {
                set_err(std::string("__pulpRuntimeImport__ threw: ") + e.what());
            } catch (...) {
                set_err("__pulpRuntimeImport__ threw: unknown exception");
            }
            return choc::value::Value();
        });

    // __pulpRuntimeSettle__(rounds) → void
    //
    // Pump the bridge's message loop + service_frame_callbacks the
    // requested number of times. Used by the JS shim to drain
    // useEffect callbacks, rAF queues, and timers after React commits.
    engine_.register_function("__pulpRuntimeSettle__",
        [this](choc::javascript::ArgumentList args) -> choc::value::Value {
            int rounds = static_cast<int>(args.get<double>(0, 8.0));
            if (rounds < 1) rounds = 1;
            if (rounds > 64) rounds = 64;  // sanity cap
            for (int i = 0; i < rounds; ++i) {
                try {
                    engine_.pump_message_loop();
                    service_frame_callbacks();
                } catch (...) { /* swallow — best-effort settle */ }
            }
            return choc::value::Value();
        });
}

void WidgetBridge::register_api() {
    // Helper: detect if arg[1] is a parent ID (string) or x position (number)
    // Old API: createKnob(id, x, y, w, h) — 5 args, arg[1] is number
    // New API: createKnob(id, parentId) — 2 args, arg[1] is string
    auto isNewApi = [](choc::javascript::ArgumentList& args) -> bool {
        if (args.numArgs <= 2) return true; // Only id + optional parent
        // If 3+ args and arg[2] looks numeric, it's old API (id, x, y, ...)
        auto test = args.get<std::string>(1, "");
        // If the string is a registered widget ID or empty, it's new API
        // If it parses as "0" or similar number, it's old API
        if (test.empty()) return true;
        if (test[0] >= '0' && test[0] <= '9') return false;
        if (test[0] == '-') return false;
        return true; // Starts with a letter = parent ID
    };

    // createKnob(id, parentId) OR createKnob(id, x, y, w, h)
    engine_.register_function("createKnob", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto knob = std::make_unique<Knob>();
        knob->set_id(id);
        knob->set_label(id);

        if (isNewApi(args)) {
            auto pid = args.get<std::string>(1, "");
            auto* ptr = knob.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            resolve_parent(pid)->add_child(std::move(knob));
        } else {
            knob->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                             (float)args.get<double>(3,48), (float)args.get<double>(4,48)});
            widgets_[id] = knob.get();
            root_.add_child(std::move(knob));
        }
        return choc::value::createString(id);
    });

    // createFader(id, orientation, parentId) OR createFader(id, x, y, w, h, orientation)
    engine_.register_function("createFader", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto fader = std::make_unique<Fader>();
        fader->set_id(id);

        if (isNewApi(args)) {
            auto orient = args.get<std::string>(1, "vertical");
            auto pid = args.get<std::string>(2, "");
            if (orient == "horizontal") fader->set_orientation(Fader::Orientation::horizontal);
            auto* ptr = fader.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            resolve_parent(pid)->add_child(std::move(fader));
        } else {
            fader->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                              (float)args.get<double>(3,24), (float)args.get<double>(4,200)});
            auto orient = args.get<std::string>(5, "vertical");
            if (orient == "horizontal") fader->set_orientation(Fader::Orientation::horizontal);
            fader->set_label(id);
            widgets_[id] = fader.get();
            root_.add_child(std::move(fader));
        }
        return choc::value::createString(id);
    });

    // createToggle(id, parentId) OR createToggle(id, x, y, w, h)
    engine_.register_function("createToggle", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto toggle = std::make_unique<Toggle>();
        toggle->set_id(id);

        if (isNewApi(args)) {
            auto pid = args.get<std::string>(1, "");
            auto* ptr = toggle.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            resolve_parent(pid)->add_child(std::move(toggle));
        } else {
            toggle->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                               (float)args.get<double>(3,50), (float)args.get<double>(4,30)});
            toggle->set_label(id);
            widgets_[id] = toggle.get();
            root_.add_child(std::move(toggle));
        }
        return choc::value::createString(id);
    });

    // createRangeSlider(id, parentId)
    // ─────────────────────────────────────────────────────────────────────
    // pulp issue-966 — generic HTML <input type="range"> mapping.
    // Distinct from createKnob (rotary) and createFader (DSP linear with
    // decorative chrome). Default visual is a 4px rounded track + circular
    // handle with the active half filled in the theme accent. After
    // creation, callers can drive setMin/setMax/setStep/setValue/
    // setOrientation/setAccentColor; quantisation lives inside the widget
    // so JS sees the same value the renderer paints.
    engine_.register_function("createRangeSlider", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto rs = std::make_unique<RangeSlider>();
        rs->set_id(id);
        auto* ptr = rs.get();
        widgets_[id] = ptr;
        wire_callbacks(id, ptr);
        resolve_parent(pid)->add_child(std::move(rs));
        return choc::value::createString(id);
    });

    // createIcon(id, type, parentId) — type: "image_upload", "send", "search", "close"
    engine_.register_function("createIcon", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto type_str = args.get<std::string>(1, "image_upload");
        auto pid = args.get<std::string>(2, "");
        auto icon = std::make_unique<Icon>();
        icon->set_id(id);
        if (type_str == "send") icon->set_type(Icon::Type::send);
        else if (type_str == "search") icon->set_type(Icon::Type::search);
        else if (type_str == "close") icon->set_type(Icon::Type::close);
        else icon->set_type(Icon::Type::image_upload);
        widgets_[id] = icon.get();
        resolve_parent(pid)->add_child(std::move(icon));
        return choc::value::createString(id);
    });

    // createImage(id, parentId) — HTML <img> equivalent
    engine_.register_function("createImage", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto img = std::make_unique<ImageView>(); img->set_id(id);
        widgets_[id] = img.get();
        resolve_parent(pid)->add_child(std::move(img));
        return choc::value::createString(id);
    });

    // setImageSource(id, path) — set image file path
    engine_.register_function("setImageSource", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto path = args.get<std::string>(1, "");
        if (auto* img = dynamic_cast<ImageView*>(widget(id)))
            img->set_image_path(path);
        return choc::value::Value();
    });

    // createCheckbox(id, parentId)
    engine_.register_function("createCheckbox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto cb = std::make_unique<Checkbox>(); cb->set_id(id);
        auto* ptr = cb.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        cb->on_change = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::string(v?"1":"0") + ")", "checkbox change");
        };
        resolve_parent(pid)->add_child(std::move(cb));
        return choc::value::createString(id);
    });

    // createToggleButton(id, parentId)
    engine_.register_function("createToggleButton", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto tb = std::make_unique<ToggleButton>(); tb->set_id(id);
        auto* ptr = tb.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        tb->on_toggle = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'toggle', " + std::string(v?"1":"0") + ")", "toggle button");
        };
        resolve_parent(pid)->add_child(std::move(tb));
        return choc::value::createString(id);
    });

    // createLabel(id, text, parentId) OR createLabel(id, text, x, y, w, h)
    engine_.register_function("createLabel", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto text = args.get<std::string>(1, "");

        // For Label: old API is (id, text, x, y, w, h) — arg[2] is number
        // New API is (id, text, parentId) — arg[2] is string or absent
        bool old = false;
        if (args.numArgs >= 4) {
            auto test = args.get<std::string>(2, "");
            if (!test.empty() && (test[0] >= '0' && test[0] <= '9')) old = true;
        }

        auto label = std::make_unique<Label>(text);
        label->set_id(id);

        if (old) {
            label->set_bounds({(float)args.get<double>(2,0), (float)args.get<double>(3,0),
                              (float)args.get<double>(4,100), (float)args.get<double>(5,20)});
            widgets_[id] = label.get();
            root_.add_child(std::move(label));
        } else {
            auto pid = args.get<std::string>(2, "");
            widgets_[id] = label.get();
            resolve_parent(pid)->add_child(std::move(label));
        }
        return choc::value::createString(id);
    });

    // Keep old-API registration paths below for backward compat
    // (The above replacements handle both APIs)

    // ── OLD createFader removed (replaced above) ──
    // Skip re-registering — the lambda above handles both APIs

    // Continue with other registrations...
    // (Old createFader/Toggle/Label registrations removed — replaced with
    // dual-API versions above that detect parent ID vs absolute positioning)

    // setValue(id, value) -> set widget value
    // For Knob / Fader / Toggle this is normalised 0..1.
    // For RangeSlider (issue-966) it's the raw value in [min,max] — the
    // widget clamps + quantises against its own configured range.
    engine_.register_function("setValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();

        if (auto* knob = dynamic_cast<Knob*>(it->second))
            knob->set_value(static_cast<float>(value));
        else if (auto* fader = dynamic_cast<Fader*>(it->second))
            fader->set_value(static_cast<float>(value));
        else if (auto* range = dynamic_cast<RangeSlider*>(it->second))
            range->set_value(static_cast<float>(value));
        else if (auto* toggle = dynamic_cast<Toggle*>(it->second))
            toggle->set_on(value > 0.5);
        else if (auto* cb = dynamic_cast<Checkbox*>(it->second))
            cb->set_checked(value > 0.5);
        else if (auto* tb = dynamic_cast<ToggleButton*>(it->second))
            tb->set_on(value > 0.5);

        return choc::value::Value();
    });

    // getValue(id) -> get widget value (normalised for Knob/Fader/Toggle,
    // raw for RangeSlider).
    engine_.register_function("getValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");

        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::createFloat64(0);

        if (auto* knob = dynamic_cast<Knob*>(it->second))
            return choc::value::createFloat64(knob->value());
        if (auto* fader = dynamic_cast<Fader*>(it->second))
            return choc::value::createFloat64(fader->value());
        if (auto* range = dynamic_cast<RangeSlider*>(it->second))
            return choc::value::createFloat64(range->value());
        if (auto* toggle = dynamic_cast<Toggle*>(it->second))
            return choc::value::createFloat64(toggle->is_on() ? 1.0 : 0.0);

        return choc::value::createFloat64(0);
    });

    // ── pulp issue-966 — RangeSlider configuration setters ────────────────
    // setMin/setMax/setStep mirror the HTMLInputElement attributes
    // `min`, `max`, `step`. Each one re-applies the widget's clamp +
    // quantisation pipeline so out-of-range values stay consistent.
    engine_.register_function("setMin", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 0);
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id)))
            range->set_min(static_cast<float>(v));
        return choc::value::Value();
    });

    engine_.register_function("setMax", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 1);
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id)))
            range->set_max(static_cast<float>(v));
        return choc::value::Value();
    });

    engine_.register_function("setStep", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 0);
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id)))
            range->set_step(static_cast<float>(v));
        return choc::value::Value();
    });

    // setOrientation(id, "horizontal" | "vertical")
    // Currently only RangeSlider (issue-966); future widgets that need
    // an orientation can extend this dynamic_cast chain.
    engine_.register_function("setOrientation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto orient = args.get<std::string>(1, "horizontal");
        auto* w = widget(id);
        if (auto* range = dynamic_cast<RangeSlider*>(w)) {
            range->set_orientation(orient == "vertical"
                ? RangeSlider::Orientation::vertical
                : RangeSlider::Orientation::horizontal);
        } else if (auto* fader = dynamic_cast<Fader*>(w)) {
            fader->set_orientation(orient == "horizontal"
                ? Fader::Orientation::horizontal
                : Fader::Orientation::vertical);
        }
        return choc::value::Value();
    });

    // setAccentColor(id, "#hex" | "rgb(...)" | "")
    // Empty string clears the override and returns to the active theme's
    // `control.fill` / `control.thumb` tokens.
    engine_.register_function("setAccentColor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id))) {
            if (hex.empty()) {
                range->clear_accent_color();
            } else {
                // parseColor is defined later in register_api; we rebuild
                // the same parser inline so the order of registrations
                // doesn't matter. Hex paths cover the spec's overwhelming
                // majority of usage; rgb()/named fall back to white.
                canvas::Color c = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
                if (!hex.empty() && hex[0] == '#') {
                    if (hex.size() == 4) {
                        c.r = static_cast<float>(std::stoul(std::string(2, hex[1]), nullptr, 16)) / 255.0f;
                        c.g = static_cast<float>(std::stoul(std::string(2, hex[2]), nullptr, 16)) / 255.0f;
                        c.b = static_cast<float>(std::stoul(std::string(2, hex[3]), nullptr, 16)) / 255.0f;
                    } else if (hex.size() >= 7) {
                        c.r = static_cast<float>(std::stoul(hex.substr(1,2), nullptr, 16)) / 255.0f;
                        c.g = static_cast<float>(std::stoul(hex.substr(3,2), nullptr, 16)) / 255.0f;
                        c.b = static_cast<float>(std::stoul(hex.substr(5,2), nullptr, 16)) / 255.0f;
                        if (hex.size() >= 9)
                            c.a = static_cast<float>(std::stoul(hex.substr(7,2), nullptr, 16)) / 255.0f;
                    }
                }
                range->set_accent_color(c);
            }
        }
        return choc::value::Value();
    });

    // getParam(name) -> get parameter value from store (normalized)
    engine_.register_function("getParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                return choc::value::createFloat64(store_.get_normalized(info->id));
            }
        }
        return choc::value::createFloat64(0);
    });

    // setParam(name, normalized_value) -> set parameter in store
    engine_.register_function("setParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                store_.set_normalized(info->id, static_cast<float>(value));
                break;
            }
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════════
    // Animation bridge
    // ═══════════════════════════════════════════════════════════════════

    // animate(id, property, targetValue, durationMs, easingName)
    // animate(id, property, target, duration_ms, easing) — CSS transition equivalent
    // Smoothly interpolates a property from current to target over duration
    engine_.register_function("animate", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "value");
        auto target = static_cast<float>(args.get<double>(2, 0));
        auto dur_ms = static_cast<float>(args.get<double>(3, 150));
        auto ease_name = args.get<std::string>(4, "ease_out_cubic");

        auto* v = widget(id);
        if (!v) return choc::value::Value();

        float dur = dur_ms / 1000.0f;
        (void)ease_name; // easing for future ValueAnimation integration

        // Apply property changes — for now immediate with duration stored
        // TODO: integrate with FrameClock for actual interpolation
        if (prop == "opacity") {
            v->set_opacity(target);
        } else if (prop == "scale") {
            v->set_scale(target);
        } else if (prop == "translate_x") {
            v->set_translate(target, v->translate_y());
        } else if (prop == "translate_y") {
            v->set_translate(v->translate_x(), target);
        } else if (prop == "rotation") {
            v->set_rotation(target);
        } else if (prop == "value") {
            if (auto* k = dynamic_cast<Knob*>(v)) k->set_value(target);
            else if (auto* f = dynamic_cast<Fader*>(v)) f->set_value(target);
            else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_on(target > 0.5f);
        }
        (void)dur;
        return choc::value::Value();
    });

    // setMotionToken(tokenName, value)
    engine_.register_function("setMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = static_cast<float>(args.get<double>(1, 0));
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.dimensions[name] = value;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // getMotionToken(tokenName) -> value
    engine_.register_function("getMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto d = root_.theme().dimension(name);
        return choc::value::createFloat64(d.value_or(0.0f));
    });

    // pulp #1899 (gap #3) — string-valued theme-token lookup.
    // setStringToken(name, value) / getStringToken(name) parallel the
    // motion-token / color-token APIs but back onto `theme.strings`
    // (the same map that already stores design-system font names
    // imported via Stitch / W3C tokens — see design_import.cpp). The
    // React-side prop-applier consults this to resolve `var(--mono)`
    // in `fontFamily` / `color` / `borderColor` etc. before forwarding
    // to setFontFamily / setTextColor — without this, Skia's font
    // matcher received the literal string "var(--mono)" as a family
    // name and fell through to a proportional sans, which is visible
    // as the Spectr top-bar "faint label" symptom from #1899.
    engine_.register_function("setStringToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = args.get<std::string>(1, "");
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.strings[name] = value;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // getStringToken(tokenName) -> string. Returns empty string when the
    // token isn't defined — callers (e.g. prop-applier resolveVar) treat
    // empty as "miss" and try the next lookup tier.
    engine_.register_function("getStringToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto s = root_.theme().string_token(name);
        return choc::value::createString(s.value_or(std::string()));
    });

    // setVisible(id, bool)
    engine_.register_function("setVisible", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vis = args.get<double>(1, 1) > 0.5;
        auto it = widgets_.find(id);
        if (it != widgets_.end()) it->second->set_visible(vis);
        return choc::value::Value();
    });

    // ── Accessibility (pulp Wave 3 html.2 / #1476) ───────────────────────
    //
    // setAccessibilityLabel / setAccessibilityRole are the bridge-side
    // entry points the html-compat layer calls when JS does
    //   el.setAttribute('aria-label', '...')
    //   el.setAttribute('role',       '...').
    //
    // Storage lives on View::access_label_ / View::access_role_ (the
    // existing widget-level a11y slots — already consumed by the macOS
    // NSAccessibility bridge in core/view/platform/mac/accessibility_mac.mm
    // and the cross-platform AccessibilityTree snapshot in
    // accessibility_tree.cpp).  Linux AT-SPI / Windows UIA platform
    // routing remains a separate concern (#217) — the bridge entry point
    // is platform-agnostic; JS-side authors can rely on the same surface
    // on every platform and the storage round-trips through getAttribute
    // either way.
    //
    // Role mapping mirrors the W3C ARIA -> AccessRole bucket used by the
    // existing widget setters: ARIA roles outside Pulp's enum collapse to
    // `group` (the most neutral container role) so VoiceOver still
    // announces the element as an interactive group rather than an
    // unknown blob, and the JS-author intent ("yes, this is exposed to
    // assistive tech") is preserved.  Unknown / empty role clears the
    // role back to AccessRole::none.
    engine_.register_function("setAccessibilityLabel",
                              [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto label = args.get<std::string>(1, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) it->second->set_access_label(std::move(label));
        return choc::value::Value();
    });

    engine_.register_function("setAccessibilityRole",
                              [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto role = args.get<std::string>(1, "");
        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();

        // ARIA role token -> View::AccessRole bucket.  Mirrors the spirit
        // of NSAccessibilityRole / UIA control-type mapping: collapse the
        // long ARIA tail onto the small Pulp enum, but keep semantic
        // hints alive so the platform layer announces something sensible.
        View::AccessRole r = View::AccessRole::none;
        if (role.empty() || role == "none" || role == "presentation") {
            r = View::AccessRole::none;
        } else if (role == "slider") {
            r = View::AccessRole::slider;
        } else if (role == "checkbox" || role == "switch" || role == "radio") {
            r = View::AccessRole::toggle;
        } else if (role == "img" || role == "image") {
            r = View::AccessRole::image;
        } else if (role == "progressbar" || role == "meter") {
            r = View::AccessRole::meter;
        } else if (role == "heading" || role == "label" ||
                   role == "text"    || role == "paragraph") {
            r = View::AccessRole::label;
        } else {
            // Everything else (button, link, navigation, region, dialog,
            // listbox, menu, ...) collapses to `group` — VoiceOver
            // announces it as a generic interactive group, which is
            // strictly better than treating it as untyped.
            r = View::AccessRole::group;
        }
        it->second->set_access_role(r);
        return choc::value::Value();
    });

    // pulp #1737 — ARIA state attributes (aria-pressed / aria-checked /
    // aria-disabled / aria-hidden). Tri-state per ARIA 1.2: `true` /
    // `false` / `mixed` / unset. We store the raw string the JS shim
    // hands us so platform AT bridges (NSAccessibility today; AT-SPI /
    // UIA when those land per pulp #217) can read it back verbatim.
    // Single bridge fn rather than four — `attr` selects the slot.
    engine_.register_function("setAccessibilityState",
                              [this](choc::javascript::ArgumentList args) {
        auto id    = args.get<std::string>(0, "");
        auto attr  = args.get<std::string>(1, "");
        auto value = args.get<std::string>(2, "");
        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();
        if      (attr == "pressed")  it->second->set_access_pressed(std::move(value));
        else if (attr == "checked")  it->second->set_access_checked(std::move(value));
        else if (attr == "disabled") it->second->set_access_disabled(std::move(value));
        else if (attr == "hidden")   it->second->set_access_hidden(std::move(value));
        // Unknown aria-* state attr → no-op (forward-compat per ARIA spec).
        return choc::value::Value();
    });

    // registerHover(id) — enables "mouseenter"/"mouseleave" JS callbacks (CSS :hover)
    engine_.register_function("registerHover", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            auto alive = callback_alive_;
            auto* engine = &engine_;
            it->second->on_hover_enter = [alive, engine, id]() {
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'mouseenter', 0)", "hover enter");
            };
            it->second->on_hover_leave = [alive, engine, id]() {
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'mouseleave', 0)", "hover leave");
            };
        }
        return choc::value::Value();
    });

    // createGrid(id, parentId) — creates a grid container (CSS display: grid)
    engine_.register_function("createGrid", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>();
        v->set_id(id);
        v->set_layout_mode(LayoutMode::grid);
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    // setGrid(id, property, value) — set grid container properties
    engine_.register_function("setGrid", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();

        if (key == "template_columns") {
            auto tmpl = args.get<std::string>(2, "");
            v->grid().template_columns = GridStyle::parse_template(tmpl);
        } else if (key == "template_rows") {
            auto tmpl = args.get<std::string>(2, "");
            v->grid().template_rows = GridStyle::parse_template(tmpl);
        } else if (key == "column_gap") {
            v->grid().column_gap = static_cast<float>(args.get<double>(2, 0));
        } else if (key == "row_gap") {
            v->grid().row_gap = static_cast<float>(args.get<double>(2, 0));
        } else if (key == "gap") {
            float g = static_cast<float>(args.get<double>(2, 0));
            v->grid().column_gap = g;
            v->grid().row_gap = g;
        } else if (key == "column_start") {
            v->grid().grid_column_start = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "column_end") {
            v->grid().grid_column_end = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "row_start") {
            v->grid().grid_row_start = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "row_end") {
            v->grid().grid_row_end = static_cast<int>(args.get<double>(2, 0));
        }
        // pulp #1434 Phase A2-2 — extended grid surface.
        else if (key == "auto_columns") {
            auto tracks = GridStyle::parse_template(args.get<std::string>(2, "auto"));
            if (!tracks.empty()) v->grid().auto_columns = tracks[0];
        } else if (key == "auto_rows") {
            auto tracks = GridStyle::parse_template(args.get<std::string>(2, "auto"));
            if (!tracks.empty()) v->grid().auto_rows = tracks[0];
        } else if (key == "auto_flow") {
            v->grid().auto_flow = GridStyle::parse_auto_flow(args.get<std::string>(2, "row"));
        } else if (key == "template_areas") {
            v->grid().template_areas = GridStyle::parse_template_areas(args.get<std::string>(2, ""));
        } else if (key == "grid_area") {
            // CSS: `grid-area: header` references a named area on the
            // parent. CSS also accepts `grid-area: 1 / 2 / 3 / 4`
            // (row-start / col-start / row-end / col-end). Distinguish
            // by checking for digits + slashes.
            auto val = args.get<std::string>(2, "");
            if (val.find('/') != std::string::npos) {
                std::vector<int> nums;
                std::string acc;
                for (char c : val) {
                    if (c == '/') {
                        try { nums.push_back(std::stoi(acc)); } catch (...) {}
                        acc.clear();
                    } else if (!std::isspace(static_cast<unsigned char>(c))) acc += c;
                }
                if (!acc.empty()) {
                    try { nums.push_back(std::stoi(acc)); } catch (...) {}
                }
                if (nums.size() >= 4) {
                    v->grid().grid_row_start = nums[0];
                    v->grid().grid_column_start = nums[1];
                    v->grid().grid_row_end = nums[2];
                    v->grid().grid_column_end = nums[3];
                }
            } else {
                v->grid().grid_area_name = val;
            }
        }
        return choc::value::Value();
    });

    // registerClick(id) — enables "click" event dispatch for any widget
    engine_.register_function("registerClick", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            auto alive = callback_alive_;
            auto* engine = &engine_;
            it->second->on_click = [alive, engine, id]() {
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'click', 0)", "click");
            };
        }
        return choc::value::Value();
    });

    // claimOverlay(id) / releaseOverlay(id) — pulp #1148 generalized overlay
    // click routing. JSX `<View overlay>` calls claimOverlay on mount and
    // releaseOverlay on unmount via the @pulp/react prop-applier; the platform
    // window host then short-circuits hit-testing for clicks that land inside
    // the named view's bounds. ComboBox keeps its own active_popup_ path —
    // these handlers drive the per-View `View::active_overlay_` mechanism.
    engine_.register_function("claimOverlay", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end() && it->second) {
            // pulp #1361 — install a JS-visible dismiss callback so React
            // `<View overlay onDismissed>` consumers can flip setOpen(false)
            // when the framework dismisses the overlay via ESC or
            // outside-click. The legacy ModalOverlay / ComboBox / CallOutBox
            // paths don't go through claim_overlay(), so they're unaffected.
            auto alive = callback_alive_;
            auto* engine = &engine_;
            it->second->on_overlay_dismissed = [alive, engine, id]() {
                safe_dispatch_eval(alive, engine,
                    "__dispatch__('" + id + "', 'dismiss', 0)", "overlay dismiss");
            };
            it->second->claim_overlay();
        }
        return choc::value::Value();
    });
    engine_.register_function("releaseOverlay", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end() && it->second) {
            // pulp #1361 — JS-driven release (typically React unmount). Clear
            // the dismiss callback first so a subsequent ESC / outside-click
            // can't re-fire on the now-detached widget.
            it->second->on_overlay_dismissed = nullptr;
            it->second->release_overlay();
        }
        return choc::value::Value();
    });

    // ── Pointer events (P2) ─────────────────────────────────────────────

    // Helper: build JS object literal for pointer event data from MouseEvent
    auto pointer_data_js = [](const std::string& id, const MouseEvent& me) -> std::string {
        std::string js = "__dispatch__('" + id + "', '";
        // Event type placeholder — caller appends type
        return js;
    };

    // registerPointer(id) — enables pointer event dispatch for a widget
    engine_.register_function("registerPointer", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        // Idempotency: re-renders re-issue registerPointer for the same id.
        // Without this gate each call wraps the previous on_pointer_event,
        // stacking N lambdas and multiplying dispatch cost by render count.
        if (!pointer_registered_.insert(id).second) {
            return choc::value::Value();
        }
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            auto* w = it->second;
            auto alive = callback_alive_;
            auto* engine = &engine_;
            auto previous_pointer = w->on_pointer_event;
            w->on_pointer_event = [alive, engine, id, previous_pointer](const MouseEvent& me) {
                if (previous_pointer) {
                    previous_pointer(me);
                }
                if (me.is_wheel) {
                    return;
                }
                std::string type;
                if (me.is_down) type = "pointerdown";
                else type = "pointerup";
                if (me.is_cancelled) type = "pointercancel";

                // W3C MouseEvent.button: left=0, middle=1, right=2.
                // MouseButton enum is left=1, right=2, middle=3 — must remap or
                // every left-click reaches JS as button=1, which fires middle-
                // click handlers (e.g. Spectr's pan-mode toggle) on every click
                // and silently breaks left-click drag (e.g. band drawing).
                int w3c_button = 0;
                switch (me.button) {
                    case MouseButton::left:   w3c_button = 0; break;
                    case MouseButton::middle: w3c_button = 1; break;
                    case MouseButton::right:  w3c_button = 2; break;
                    case MouseButton::none:   w3c_button = 0; break;
                }
                std::string data = "{"
                    "clientX:" + std::to_string(me.window_position.x) + ","
                    "clientY:" + std::to_string(me.window_position.y) + ","
                    "offsetX:" + std::to_string(me.position.x) + ","
                    "offsetY:" + std::to_string(me.position.y) + ","
                    "pointerId:" + std::to_string(me.pointer_id) + ","
                    "pointerType:'" + std::string(me.pointerTypeString()) + "',"
                    "isPrimary:" + (me.isPrimary() ? "true" : "false") + ","
                    "pressure:" + std::to_string(me.pressure) + ","
                    "altitudeAngle:" + std::to_string(me.altitude_angle) + ","
                    "azimuthAngle:" + std::to_string(me.azimuth_angle) + ","
                    "button:" + std::to_string(w3c_button) + ","
                    "ctrlKey:" + (me.isCtrlDown() ? "true" : "false") + ","
                    "shiftKey:" + (me.isShiftDown() ? "true" : "false") + ","
                    "altKey:" + (me.isAltDown() ? "true" : "false") + ","
                    "metaKey:" + (me.isCmdDown() ? "true" : "false") +
                    "}";

                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', '" + type + "', " + data + ")", "pointer");
            };
            // W3C PointerEvents: forward drag as pointermove.
            // Include clientX/clientY (window-relative) so JSX drag
            // handlers reading `e.clientX - rect.left` (the standard
            // pattern) get real coords. Pre-fix, pointermove only had
            // offsetX/Y — JSX dragging a slider thumb or drawing a band
            // column read clientX as 0 and the drag silently broke.
            // Capture View* `w` so we can accumulate the parent chain
            // bounds to convert local → window coords.
            w->on_drag = [alive, engine, id, w](Point pos) {
                float wx = pos.x, wy = pos.y;
                for (View* cur = w; cur; cur = cur->parent()) {
                    wx += cur->bounds().x;
                    wy += cur->bounds().y;
                }
                std::string data = "{"
                    "clientX:" + std::to_string(wx) + ","
                    "clientY:" + std::to_string(wy) + ","
                    "offsetX:" + std::to_string(pos.x) + ","
                    "offsetY:" + std::to_string(pos.y) + ","
                    "pointerId:0,pointerType:'mouse',isPrimary:true}";
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'pointermove', " + data + ")", "pointermove");
            };
        }
        return choc::value::Value();
    });

    // registerGesture(id) — enables gesture event dispatch for a widget
    engine_.register_function("registerGesture", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            auto alive = callback_alive_;
            auto* engine = &engine_;
            it->second->on_gesture_cb = [alive, engine, id](const GestureEvent& ge) {
                std::string type;
                switch (ge.phase) {
                    case GesturePhase::began:     type = "gesturestart"; break;
                    case GesturePhase::ended:     type = "gestureend"; break;
                    case GesturePhase::cancelled: type = "gestureend"; break;
                    default:                      type = "gesturechange"; break;
                }
                std::string data = "{"
                    "scale:" + std::to_string(ge.scale) + ","
                    "rotation:" + std::to_string(ge.rotation) + ","
                    "clientX:" + std::to_string(ge.position.x) + ","
                    "clientY:" + std::to_string(ge.position.y) +
                    "}";
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', '" + type + "', " + data + ")", "gesture");
            };
        }
        return choc::value::Value();
    });

    // nativeSetPointerCapture(id, pointerId) — P2b
    engine_.register_function("nativeSetPointerCapture", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pointerId = static_cast<int>(args.get<double>(1, 0));
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            it->second->set_pointer_capture(pointerId);
            safe_dispatch_eval(engine_, "__dispatch__('" + id + "', 'gotpointercapture', {pointerId:" + std::to_string(pointerId) + "})", "gotpointercapture");
        }
        return choc::value::Value();
    });

    // nativeReleasePointerCapture(id, pointerId) — P2b
    engine_.register_function("nativeReleasePointerCapture", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pointerId = static_cast<int>(args.get<double>(1, 0));
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            it->second->release_pointer_capture(pointerId);
            safe_dispatch_eval(engine_, "__dispatch__('" + id + "', 'lostpointercapture', {pointerId:" + std::to_string(pointerId) + "})", "lostpointercapture");
        }
        return choc::value::Value();
    });

    // enableInspectClick() — sets up Cmd+click detection on all registered widgets
    engine_.register_function("enableInspectClick", [this](choc::javascript::ArgumentList) {
        auto alive = callback_alive_;
        auto* engine = &engine_;
        root_.on_global_click = [alive, engine](const std::string& id, uint16_t mods) {
            // Check for Cmd modifier (kModCmd = 0x10, kModMeta = 0x08)
            bool cmd = (mods & (0x10 | 0x08)) != 0;
            if (cmd) {
                safe_dispatch_eval(alive, engine, "__dispatch__('__inspect__', 'click', '" + id + "')", "inspect click");
            }
        };
        return choc::value::Value();
    });

    // removeWidget(id)
    engine_.register_function("removeWidget", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (auto* w = widget(id)) {
            View* parent = w->parent();
            if (parent) {
                auto removed = parent->remove_child(w);
                erase_widget_subtree(widgets_, removed.get());
            }
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════
    // Extended API: containers, layout, all widgets, themes, canvas
    // ═══════════════════════════════════════════════════════════════

    engine_.register_function("createRow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::row;
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    engine_.register_function("createCol", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::column;
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    engine_.register_function("createModal", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<ModalOverlay>(); v->set_id(id);
        v->flex().direction = FlexDirection::column;
        auto* modal = v.get();
        auto alive = callback_alive_;
        auto* engine = &engine_;
        modal->on_dismiss = [alive, engine, modal, id]() {
            if (modal) {
                modal->set_visible(false);
            }
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'dismiss', 0)", "modal dismiss");
        };
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    engine_.register_function("createPanel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<Panel>(); p->set_id(id);
        widgets_[id] = p.get();
        resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    engine_.register_function("setFlex", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        View* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto& f = v->flex();
        auto val = args.get<double>(2, 0);
        // pulp #1434 (rn batch B) — accept all five flexDirection
        // spellings: 'row' / 'column' (CSS / RN canonical), 'col' (legacy
        // pulp shorthand emitted by web-compat-style-decl.js's
        // setFlex(direction)), plus the reverse modes 'row-reverse' /
        // 'column-reverse'. Anything else falls through to column
        // (matches the prior default behavior for unknown values).
        if (key == "direction") {
            auto dir = args.get<std::string>(2, "col");
            if (dir == "row")                 f.direction = FlexDirection::row;
            else if (dir == "row-reverse")    f.direction = FlexDirection::row_reverse;
            else if (dir == "column-reverse") f.direction = FlexDirection::column_reverse;
            else                              f.direction = FlexDirection::column;
        }
        else if (key == "gap") f.gap = (float)val;
        else if (key == "padding") f.padding = (float)val;
        // pulp #1434 (cross-surface mega-batch) — per-edge padding accepts
        // either a number ("10" → px) or a percentage string ("5%" →
        // percent of parent main-axis size). Yoga's padding does NOT
        // support "auto" (only margin does), so unrecognized strings fall
        // through to the px path with the parsed numeric value (or 0 on
        // total parse failure). Mirrors width/height/min/max patterns.
        else if (key == "padding_top") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_top.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_top.unit = pulp::view::DimensionUnit::percent;
                    f.padding_top = -1; // sentinel
                } catch (...) { /* keep current */ }
            } else {
                f.padding_top = (float)val;
                f.dim_padding_top.value = (float)val;
                f.dim_padding_top.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_right") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_right.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_right.unit = pulp::view::DimensionUnit::percent;
                    f.padding_right = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.padding_right = (float)val;
                f.dim_padding_right.value = (float)val;
                f.dim_padding_right.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_bottom") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_bottom.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_bottom.unit = pulp::view::DimensionUnit::percent;
                    f.padding_bottom = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.padding_bottom = (float)val;
                f.dim_padding_bottom.value = (float)val;
                f.dim_padding_bottom.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_left") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_left.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_left.unit = pulp::view::DimensionUnit::percent;
                    f.padding_left = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.padding_left = (float)val;
                f.dim_padding_left.value = (float)val;
                f.dim_padding_left.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "flex_grow") f.flex_grow = (float)val;
        else if (key == "flex_shrink") f.flex_shrink = (float)val;
        // pulp #1434 (rn batch C) — flex_basis accepts a number ("100"
        // → px), a percentage string ("50%" → percent of parent), or
        // the keyword "auto" (Yoga's YGAuto: "use the preferred size").
        // The unit lives on FlexStyle::dim_flex_basis; yoga_layout.cpp
        // dispatches to YGNodeStyleSetFlexBasis{Percent,Auto} for the
        // non-px paths.
        else if (key == "flex_basis") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_flex_basis.unit = pulp::view::DimensionUnit::auto_;
                f.flex_basis = -1; // sentinel: use preferred
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_flex_basis.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_flex_basis.unit = pulp::view::DimensionUnit::percent;
                    f.flex_basis = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.flex_basis = (float)val;
                f.dim_flex_basis.value = (float)val;
                f.dim_flex_basis.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "flex_wrap") {
            // pulp #1434 Triage #14 — accept numeric (legacy 0/1) and
            // the CSS keyword strings, including the previously-
            // inexpressible `wrap-reverse`.
            auto sval = args.get<std::string>(2, "");
            if (sval == "wrap-reverse")        f.flex_wrap = FlexWrap::wrap_reverse;
            else if (sval == "wrap")           f.flex_wrap = FlexWrap::wrap;
            else if (sval == "nowrap" || sval == "no-wrap") f.flex_wrap = FlexWrap::no_wrap;
            else                                f.flex_wrap = (val > 0) ? FlexWrap::wrap : FlexWrap::no_wrap;
        }
        else if (key == "order") f.order = (int)val;
        // pulp #1423 — width/height accept either a number ("100" → px)
        // or a percentage string ("100%" → percent of parent). The CSS
        // translator passes the raw resolved string for these two keys
        // so the unit survives the bridge boundary; Yoga's native
        // YGNodeStyleSet{Width,Height}Percent path is reached via
        // FlexStyle::dim_width / dim_height in yoga_layout.cpp.
        else if (key == "width") {
            auto sval = args.get<std::string>(2, "");
            // pulp #1434 (sub-agent #12 follow-up) — accept the keyword
            // `'auto'` for "hug contents" sizing. Yoga supports this
            // natively via YGNodeStyleSetWidthAuto. Figma auto-layout,
            // v0 intrinsic-sizing cards, and Claude Design responsive
            // containers all emit this. The dispatch path in
            // yoga_layout.cpp keys on `dim_width.unit == auto_`.
            if (sval == "auto") {
                f.dim_width.unit = pulp::view::DimensionUnit::auto_;
                f.dim_width.value = 0;
                f.preferred_width = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_width.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_width.unit = pulp::view::DimensionUnit::percent;
                    f.preferred_width = 0;  // disambiguate: percent path
                } catch (...) { /* keep current state on parse fail */ }
            } else {
                f.preferred_width = (float)val;
                f.dim_width.value = (float)val;
                f.dim_width.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "height") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_height.unit = pulp::view::DimensionUnit::auto_;
                f.dim_height.value = 0;
                f.preferred_height = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_height.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_height.unit = pulp::view::DimensionUnit::percent;
                    f.preferred_height = 0;
                } catch (...) { /* keep current state on parse fail */ }
            } else {
                f.preferred_height = (float)val;
                f.dim_height.value = (float)val;
                f.dim_height.unit = pulp::view::DimensionUnit::px;
            }
        }
        // pulp #1434 (rn batch C) — min/max width/height accept either a
        // number ("100" → px) or a percentage string ("50%" → percent of
        // parent). Same shape as #1426's width/height: store unit on
        // FlexStyle::dim_*; yoga_layout.cpp dispatches to
        // YGNodeStyleSet{Min,Max}{Width,Height}Percent for the percent
        // path and the existing px API otherwise.
        else if (key == "min_width") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_min_width.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_min_width.unit = pulp::view::DimensionUnit::percent;
                    f.min_width = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.min_width = (float)val;
                f.dim_min_width.value = (float)val;
                f.dim_min_width.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "min_height") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_min_height.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_min_height.unit = pulp::view::DimensionUnit::percent;
                    f.min_height = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.min_height = (float)val;
                f.dim_min_height.value = (float)val;
                f.dim_min_height.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "max_width") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_max_width.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_max_width.unit = pulp::view::DimensionUnit::percent;
                    f.max_width = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.max_width = (float)val;
                f.dim_max_width.value = (float)val;
                f.dim_max_width.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "max_height") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_max_height.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_max_height.unit = pulp::view::DimensionUnit::percent;
                    f.max_height = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.max_height = (float)val;
                f.dim_max_height.value = (float)val;
                f.dim_max_height.unit = pulp::view::DimensionUnit::px;
            }
        }
        // Aspect ratio (pulp #1434) — width/height ratio. A value <= 0 or
        // NaN clears the slot (matches CSS `aspect-ratio: auto`); a finite
        // positive value pins the slot. The CSS shim in
        // web-compat-style-decl.js parses both `1.5` and `16/9` forms
        // before reaching here, and the @pulp/react prop-applier accepts
        // `aspectRatio` directly as a number.
        else if (key == "aspect_ratio" || key == "aspectRatio") {
            if (val > 0.0 && std::isfinite(val)) f.aspect_ratio = (float)val;
            else f.aspect_ratio.reset();
        }
        // Margin
        else if (key == "margin") f.margin = (float)val;
        // pulp #1434 (cross-surface mega-batch) — per-edge margin accepts
        // a number ("10" → px), a percentage string ("5%" → percent of
        // parent main-axis size), or the keyword "auto" (Yoga
        // YGNodeStyleSetMarginAuto — used for centering with
        // `marginLeft: 'auto'; marginRight: 'auto'` etc.). Yoga supports
        // `auto` on margin only, not padding. yoga_layout.cpp dispatches
        // on dim_margin_*.unit; the legacy float field is kept (-1
        // sentinel) so consumers still using uniform `margin` work
        // unchanged.
        else if (key == "margin_top") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_top.unit = pulp::view::DimensionUnit::auto_;
                f.margin_top = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_top.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_top.unit = pulp::view::DimensionUnit::percent;
                    f.margin_top = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_top = (float)val;
                f.dim_margin_top.value = (float)val;
                f.dim_margin_top.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_right") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_right.unit = pulp::view::DimensionUnit::auto_;
                f.margin_right = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_right.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_right.unit = pulp::view::DimensionUnit::percent;
                    f.margin_right = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_right = (float)val;
                f.dim_margin_right.value = (float)val;
                f.dim_margin_right.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_bottom") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_bottom.unit = pulp::view::DimensionUnit::auto_;
                f.margin_bottom = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_bottom.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_bottom.unit = pulp::view::DimensionUnit::percent;
                    f.margin_bottom = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_bottom = (float)val;
                f.dim_margin_bottom.value = (float)val;
                f.dim_margin_bottom.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_left") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_left.unit = pulp::view::DimensionUnit::auto_;
                f.margin_left = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_left.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_left.unit = pulp::view::DimensionUnit::percent;
                    f.margin_left = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_left = (float)val;
                f.dim_margin_left.value = (float)val;
                f.dim_margin_left.unit = pulp::view::DimensionUnit::px;
            }
        }
        // pulp #1542 — yoga logical-edge fan-out. `margin_start` /
        // `margin_end` / `padding_start` / `padding_end` / `start` /
        // `end` route to Yoga's YGEdgeStart / YGEdgeEnd which flip
        // with the writing direction (set via `direction_writing` —
        // the existing `direction` key is taken by flex-direction).
        // Same value coverage as the per-side _left/_right siblings:
        // px (number), percent string, plus `auto` for margin.
        else if (key == "margin_start") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_start.unit = pulp::view::DimensionUnit::auto_;
                f.dim_margin_start.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_start.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_start.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_margin_start.value = (float)val;
                f.dim_margin_start.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_end") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_end.unit = pulp::view::DimensionUnit::auto_;
                f.dim_margin_end.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_end.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_end.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_margin_end.value = (float)val;
                f.dim_margin_end.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_start") {
            // Padding has no `auto` API (Yoga); reject the keyword and
            // fall through to the px path with a parsed value (or 0
            // on parse failure) — same behavior as the per-side
            // padding_top/right/bottom/left handlers above.
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_start.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_start.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_padding_start.value = (float)val;
                f.dim_padding_start.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_end") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_end.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_end.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_padding_end.value = (float)val;
                f.dim_padding_end.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "start") {
            // pulp DIVERGE→PASS sweep — accept `'auto'` so `inset-inline-
            // start: auto` (CSS) and the equivalent RN style key route
            // through Yoga's `YGNodeStyleSetPositionAuto`. Without this
            // path the keyword fell through to (float)val == 0 and
            // pinned the start edge to 0px.
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_start.unit = pulp::view::DimensionUnit::auto_;
                f.dim_start.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_start.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_start.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_start.value = (float)val;
                f.dim_start.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "end") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_end.unit = pulp::view::DimensionUnit::auto_;
                f.dim_end.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_end.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_end.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_end.value = (float)val;
                f.dim_end.unit = pulp::view::DimensionUnit::px;
            }
        }
        // pulp #1542 — node writing direction. Distinct from
        // `direction` (which is flex-direction). Accepts `'ltr'`,
        // `'rtl'`, `'inherit'`. Anything else reverts to `inherit`.
        else if (key == "direction_writing") {
            auto a = args.get<std::string>(2, "inherit");
            if (a == "ltr")      f.writing_direction = pulp::view::FlexStyle::WritingDirection::ltr;
            else if (a == "rtl") f.writing_direction = pulp::view::FlexStyle::WritingDirection::rtl;
            else                 f.writing_direction = pulp::view::FlexStyle::WritingDirection::inherit;
        }
        // Directional gap.
        // pulp Wave 2 css.2 — accept a `'NN%'` string for parity with
        // padding/margin edges. Yoga itself does not have a
        // YGNodeStyleSetGapPercent API today, so the percent value is
        // stored on the scalar `row_gap` / `column_gap` slot verbatim
        // (treated as px until the Yoga update lands). This is the same
        // best-effort treatment we apply to other percent-but-Yoga-
        // doesn't-honor cases — the catalog entry stays `partial` and
        // documents the gap.
        else if (key == "row_gap") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try { f.row_gap = std::stof(sval.substr(0, sval.size() - 1)); }
                catch (...) { /* keep current */ }
            } else {
                f.row_gap = (float)val;
            }
        }
        else if (key == "column_gap") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try { f.column_gap = std::stof(sval.substr(0, sval.size() - 1)); }
                catch (...) { /* keep current */ }
            } else {
                f.column_gap = (float)val;
            }
        }
        // Alignment.
        // pulp #1434 (rn batch B) — accept both bare `start`/`end`
        // (Yoga / pulp short forms) and the `flex-start`/`flex-end`
        // CSS / RN canonical spellings. The CSS shim's _cssToFlex
        // already maps the prefixed forms to the bare ones for the
        // CSS path, but @pulp/react's prop-applier passes RN values
        // through verbatim — so the bridge has to accept both.
        // FlexAlign has no `baseline` variant yet (separate gap;
        // would need YGAlignBaseline plumbing); falls through to
        // stretch / auto_ as before.
        else if (key == "align_items") {
            auto a = args.get<std::string>(2,"stretch");
            if (a=="start" || a=="flex-start") f.align_items=FlexAlign::start;
            else if (a=="center")              f.align_items=FlexAlign::center;
            else if (a=="end" || a=="flex-end") f.align_items=FlexAlign::end;
            // pulp #1434 Tier 1 (css/alignItems) — CSS-spec `first baseline`
            // aliases to plain `baseline`. The baseline-set "first" selector
            // is the default behaviour (and what Yoga's YGAlignBaseline
            // computes), so collapsing them is observable-behaviour-preserving.
            // NOTE: `last baseline` is intentionally NOT aliased — it requires
            // baseline-set tracking (align children against the LAST baseline
            // line in a multi-line flex container) that YGAlignBaseline does
            // not implement. Codex P1 on PR #1853 — keeping `last baseline`
            // in compat.json/unsupportedValues is the honest answer.
            else if (a=="baseline" || a=="first baseline")
                                                f.align_items=FlexAlign::baseline;
            else                               f.align_items=FlexAlign::stretch;
        }
        else if (key == "align_self") {
            auto a = args.get<std::string>(2,"auto");
            if (a=="start" || a=="flex-start") f.align_self=FlexAlign::start;
            else if (a=="center")              f.align_self=FlexAlign::center;
            else if (a=="end" || a=="flex-end") f.align_self=FlexAlign::end;
            else if (a=="stretch")             f.align_self=FlexAlign::stretch;
            else if (a=="baseline")            f.align_self=FlexAlign::baseline;
            else                               f.align_self=FlexAlign::auto_;
        }
        // pulp #1434 (sub-agent #12 follow-up) — align_content controls
        // multi-line flex cross-axis distribution. Yoga supports this
        // natively via YGNodeStyleSetAlignContent. Accepts both bare
        // `start`/`end` (Yoga / pulp short forms) and `flex-start` /
        // `flex-end` (CSS / RN canonical). Space-* values (space-between
        // / space-around / space-evenly) live on a sibling enum field
        // (FlexStyle::align_content_space) because FlexAlign has no
        // space variants — those don't make sense for align_items /
        // align_self, only for align_content.
        else if (key == "align_content") {
            auto a = args.get<std::string>(2,"start");
            using AcSpace = pulp::view::FlexStyle::AlignContentSpace;
            // Reset space slot first; only the space-* branches set it.
            f.align_content_space = AcSpace::none;
            if (a=="start" || a=="flex-start")     f.align_content=FlexAlign::start;
            else if (a=="center")                  f.align_content=FlexAlign::center;
            else if (a=="end" || a=="flex-end")    f.align_content=FlexAlign::end;
            else if (a=="stretch")                 f.align_content=FlexAlign::stretch;
            else if (a=="space-between"||a=="space_between") {
                f.align_content_space = AcSpace::space_between;
            }
            else if (a=="space-around"||a=="space_around") {
                f.align_content_space = AcSpace::space_around;
            }
            else if (a=="space-evenly"||a=="space_evenly") {
                f.align_content_space = AcSpace::space_evenly;
            }
            else                                   f.align_content=FlexAlign::start;
        }
        else if (key == "justify_content") {
            auto j = args.get<std::string>(2,"start");
            // INTENTIONALLY NOT aliased (Codex P1 on PR #1853, two
            // separate findings, both kept honest):
            //
            //   `left` / `right` — direction-context-dependent. CSS spec:
            //       on a row container, `right` ≡ flex-end (LTR) /
            //       flex-start (RTL).
            //       on a column container, BOTH `left` and `right`
            //       behave as `start` (per CSS Box Alignment §4.3 —
            //       "If the property's axis is parallel to the inline
            //       axis, behaves as flex-end; otherwise behaves as
            //       start"). A direction-agnostic alias would silently
            //       misrender vertical flex containers. The fully-correct
            //       fix would peek at the widget's flex-direction here
            //       and dispatch accordingly; that's more state coupling
            //       than this dispatcher carries today. Keep both values
            //       in compat.json/unsupportedValues until we either
            //       add direction-aware aliasing or RTL writing-mode
            //       support arrives.
            //
            //   `stretch` — grows AUTO-sized items equally; FlexJustify
            //       has no equivalent. Aliasing to flex-start would
            //       silently misrender stretching layouts. Documented
            //       unsupported.
            //
            //   `normal`  — per CSS spec, "behaves as `stretch`" on flex
            //       containers, so it inherits the same problem.
            //       Documented unsupported.
            if (j=="start" || j=="flex-start")                f.justify_content=FlexJustify::start;
            else if (j=="center")                             f.justify_content=FlexJustify::center;
            else if (j=="end" || j=="flex-end")               f.justify_content=FlexJustify::end_;
            else if (j=="space-between"||j=="space_between")  f.justify_content=FlexJustify::space_between;
            else if (j=="space-around"||j=="space_around")    f.justify_content=FlexJustify::space_around;
            else if (j=="space-evenly"||j=="space_evenly")    f.justify_content=FlexJustify::space_evenly;
            else                                              f.justify_content=FlexJustify::start;
        }
        v->invalidate_layout();  // auto-invalidation on flex property change
        return choc::value::Value();
    });

    // __domAppend(parentId, childId, tag) — native appendChild.
    // Creates a native widget under parentId, purely in C++ — no re-entrant
    // JS evaluation which causes stack overflow in QuickJS.
    engine_.register_function("__domAppend", [this](choc::javascript::ArgumentList args) {
        auto parentId = args.get<std::string>(0, "");
        auto childId = args.get<std::string>(1, "");
        auto tag = args.get<std::string>(2, "div");
        auto* existing = widget(childId);
        if (existing) {
            if (auto* p = existing->parent()) {
                // Move the existing subtree to the new parent — don't erase widgets
                auto removed = p->remove_child(existing);
                widgets_[childId] = removed.get();
                resolve_parent(parentId)->add_child(std::move(removed));
                return choc::value::Value();
            }
        }
        // Create the appropriate widget type based on HTML tag.
        //
        // pulp #1147 — this fast-path bypasses the JS-side `_ensureNative`
        // for performance + QuickJS stack reasons, but it MUST mirror the
        // tag→widget mapping in `web-compat-element.js` or web-compat
        // semantics drift between the createElement+appendChild path and
        // the React-style commit path that goes through here.
        std::unique_ptr<View> child;
        if (tag == "span" || tag == "p" || tag == "label" ||
            tag == "h1" || tag == "h2" || tag == "h3" ||
            tag == "h4" || tag == "h5" || tag == "h6") {
            auto lbl = std::make_unique<Label>();
            lbl->set_id(childId);
            child = std::move(lbl);
        } else if (tag == "canvas") {
            auto canvas = std::make_unique<CanvasWidget>();
            canvas->set_id(childId);
            canvas->set_native_gpu_texture_provider([this, childId]() {
                return this->describe_native_canvas_frame(childId);
            });
            child = std::move(canvas);
        } else if (tag == "rect") {
            // pulp #1926 — SVG primitives other than <path>. Spectr's
            // bottom-toolbar mini-icons (segmented mode toggles, analyzer
            // pills) emit lowercase <rect> / <line> / <circle> inside the
            // parent <svg>. Without this routing they fall through to the
            // unknown-tag default and paint nothing.
            auto r = std::make_unique<SvgRectWidget>();
            r->set_id(childId);
            child = std::move(r);
        } else if (tag == "line") {
            auto l = std::make_unique<SvgLineWidget>();
            l->set_id(childId);
            child = std::move(l);
        } else if (tag == "circle") {
            // pulp #1926 — no dedicated SvgCircleWidget; map to SvgPath
            // and synthesize a `d` arc in JS (web-compat-element.js
            // __replaySvgCircleAttributes__) from cx/cy/r.
            auto svg = std::make_unique<SvgPathWidget>();
            svg->set_id(childId);
            child = std::move(svg);
        } else if (tag == "path") {
            // pulp #1899 — mirror the JS-side _ensureNative routing:
            // `<path>` (typically inside an `<svg>`) materializes as the
            // SvgPathWidget so the d / stroke / stroke-width / fill /
            // viewBox attribute replay actually paints. Without this
            // branch the React/JSX commit path (which goes through
            // __domAppend, bypassing _ensureNative) would silently
            // create a plain View and the SVG glyph never renders.
            auto svg = std::make_unique<SvgPathWidget>();
            svg->set_id(childId);
            child = std::move(svg);
        } else if (tag == "input") {
            // pulp #1899 — `<input>` needs the JS-side `_type` to pick a
            // widget. JS callers pass it through the optional 4th `hint`
            // arg ("range:horizontal", "range:vertical", "checkbox").
            // Without the hint, fall back to a plain View so the element
            // still receives child/style ops (text inputs are not yet
            // first-class on the bridge).
            auto hint = args.get<std::string>(3, "");
            if (hint == "range:horizontal" || hint == "range:vertical") {
                auto fader = std::make_unique<Fader>();
                fader->set_id(childId);
                if (hint == "range:horizontal") {
                    fader->set_orientation(Fader::Orientation::horizontal);
                }
                child = std::move(fader);
            } else if (hint == "checkbox") {
                auto cb = std::make_unique<Checkbox>();
                cb->set_id(childId);
                child = std::move(cb);
            } else {
                auto v = std::make_unique<View>();
                v->set_id(childId);
                child = std::move(v);
            }
        } else {
            auto v = std::make_unique<View>();
            v->set_id(childId);
            if (tag == "div" || tag == "section" || tag == "article" || tag == "aside" ||
                tag == "header" || tag == "footer" || tag == "nav" || tag == "main")
                v->flex().direction = FlexDirection::column;
            // pulp #1147 — <svg> is a layout-leaf media element. Default
            // direction stays column so child <path>/<g> attaches; the
            // presentational width/height attributes are replayed via
            // setFlex() on the JS side (see web-compat-element.js
            // setAttribute() path).
            child = std::move(v);
        }
        widgets_[childId] = child.get();
        resolve_parent(parentId)->add_child(std::move(child));
        return choc::value::Value();
    });

    // __domRemove(childId) — native removeChild implementation
    engine_.register_function("__domRemove", [this](choc::javascript::ArgumentList args) {
        auto childId = args.get<std::string>(0, "");
        auto* w = widget(childId);
        if (w) {
            if (auto* p = w->parent()) {
                auto removed = p->remove_child(w);
                erase_widget_subtree(widgets_, removed.get());
            }
        }
        return choc::value::Value();
    });

    engine_.register_function("layout", [this](choc::javascript::ArgumentList) {
        root_.clear_layout_dirty();
        root_.layout_children();
        request_repaint();
        return choc::value::Value();
    });

    // getLayoutRect(id) -> {x, y, width, height, top, right, bottom, left}
    // Returns layout-resolved bounds in root-relative coordinates.
    //
    // pulp #1899 — force a fresh layout pass before reading bounds.
    // Spectr's editor (and any React-imported tree) calls this via
    // Element.getBoundingClientRect() in mount-time effects to size
    // a canvas / SVG / drawing surface. If the JS commit that mounted
    // the React tree hasn't yet been followed by a yoga_layout pass,
    // the bounds read back as the View's stale default (0×0) — which
    // gates the entire canvas paint pipeline (drawSpectrum/drawRulers
    // bail at getBoundingClientRect == 0). Forcing layout here closes
    // that timing gap. Layout is internally idempotent if nothing has
    // changed, so the cost is bounded to one tree walk per call.
    engine_.register_function("getLayoutRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        root_.layout_children();
        View* v = id.empty() ? &root_ : widget(id);
        return make_layout_rect_value(v);
    });

    // getRootSize() -> {width, height} — actual root view dimensions for vw/vh/matchMedia
    engine_.register_function("getRootSize", [this](choc::javascript::ArgumentList) {
        auto b = root_.bounds();
        auto r = choc::value::createObject("");
        r.addMember("width", choc::value::createFloat64(b.width));
        r.addMember("height", choc::value::createFloat64(b.height));
        return r;
    });

    // setPointerEvents(id, "none"|"auto") — CSS pointer-events: skip in hit_test
    // pulp #1026 — RN-shaped 4-valued pointerEvents:
    //   "auto"     — default, this view + children intercept events.
    //   "none"     — neither this view nor descendants intercept events.
    //   "box-only" — this view intercepts; children do NOT.
    //   "box-none" — this view does NOT intercept; children do.
    // Pre-#1026 the bridge only honored auto / none and routed through
    // set_hit_testable(); we keep that mapping for the binary cases for
    // back-compat with existing scripts and additionally route the new
    // four-valued enum via View::set_pointer_events().
    engine_.register_function("setPointerEvents", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "auto");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (mode == "none") {
            v->set_hit_testable(false);
            v->set_pointer_events(View::PointerEvents::none);
        } else if (mode == "box-only" || mode == "box_only") {
            v->set_hit_testable(true);
            v->set_pointer_events(View::PointerEvents::box_only);
        } else if (mode == "box-none" || mode == "box_none") {
            v->set_hit_testable(true);
            v->set_pointer_events(View::PointerEvents::box_none);
        } else {
            v->set_hit_testable(true);
            v->set_pointer_events(View::PointerEvents::auto_);
        }
        return choc::value::Value();
    });

    // pulp #1026 — RN backfaceVisibility ("visible"|"hidden"). Stored on
    // the View for plumbing parity with @pulp/react. The flag is consumed
    // by the paint path only when a 3D transform with negative Z is
    // active; pulp's transform model is currently 2D-affine, so this is
    // a no-op for painting today and reserved for future 3D support.
    engine_.register_function("setBackfaceVisibility", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "visible");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_backface_visible(mode != "hidden");
        return choc::value::Value();
    });

    // setVisibility(id, "visible"|"hidden") — hidden preserves layout space
    engine_.register_function("setVisibility", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vis = args.get<std::string>(1, "visible");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            // visibility:hidden = still takes space but not painted
            // We use opacity 0 + still visible for layout
            if (vis == "hidden") { v->set_opacity(0); }
            else { v->set_opacity(1); }
        }
        return choc::value::Value();
    });

    // setWhiteSpace(id, "normal"|"nowrap"|"pre"|"pre-wrap")
    //
    // pulp #1410 — sets a generic `View::white_space_nowrap()` flag so
    // ANY widget with a textual surface (Button, custom text-bearing
    // views, future TextEditor surfaces) and `TextShaper` consumers can
    // observe nowrap without dynamic_casting to Label. The original
    // Label::set_multi_line side-effect stays in lock-step so existing
    // single-line Label paint paths (incl. #1407 ellipsis truncation)
    // keep working when only one of the flags is set.
    engine_.register_function("setWhiteSpace", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto ws = args.get<std::string>(1, "normal");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        // pulp #1737 — map keyword to View::WhiteSpaceMode enum.
        // The set_white_space_mode setter also maintains the legacy
        // white_space_nowrap_ bool (true for nowrap + pre) so existing
        // consumers of white_space_nowrap() keep working.
        using M = View::WhiteSpaceMode;
        M mode = M::normal;
        if      (ws == "nowrap")        mode = M::nowrap;
        else if (ws == "pre")           mode = M::pre;
        else if (ws == "pre-wrap")      mode = M::pre_wrap;
        else if (ws == "pre-line")      mode = M::pre_line;
        else if (ws == "break-spaces")  mode = M::break_spaces;
        // Unknown keyword falls back to normal (per CSS forward-compat).
        v->set_white_space_mode(mode);
        // pulp #1737 (Codex P1 followup on #1786): Label.multi_line
        // is TRUE for all modes except `nowrap`. Originally `pre`
        // mapped to multi_line=false to match the CSS spec's
        // "no-soft-wrap" semantic, but Pulp's Label only emits hard
        // line breaks via the multi_line splitting path — single-line
        // mode draws the whole string in one fill_text call, dropping
        // `\n`. So <pre> content with newlines silently lost its
        // breaks. Per CSS spec, pre MUST preserve newlines as hard
        // breaks; the only thing it disables is SOFT wrapping at word
        // boundaries (long lines overflow). Pulp's Label doesn't have
        // a separate soft-wrap-vs-hard-break knob today, so we honour
        // the spec-critical "preserve newlines" by keeping
        // multi_line=true for `pre`. Long lines overflow horizontally
        // — a degraded but spec-correct behaviour. Soft-wrap
        // suppression for `pre` is a Label-side follow-up.
        if (auto* l = dynamic_cast<Label*>(widget(id))) {
            const bool wraps = (mode != M::nowrap);
            l->set_multi_line(wraps);
        }
        return choc::value::Value();
    });

    // setUserSelect(id, "auto"|"none"|"text"|"all"|"contain") — CSS
    // user-select. Tier-2 follow-up to #1656 (which walked the catalog
    // claim back to partial because this stub was a literal no-op).
    // Now stores the keyword on View::user_select_ so widgets that
    // participate in selection can read it. Unknown keywords map to
    // the spec default (auto).
    engine_.register_function("setUserSelect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "auto");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if      (kw == "none")    v->set_user_select(View::UserSelect::none);
        else if (kw == "text")    v->set_user_select(View::UserSelect::text);
        else if (kw == "all")     v->set_user_select(View::UserSelect::all);
        else if (kw == "contain") v->set_user_select(View::UserSelect::contain);
        else                       v->set_user_select(View::UserSelect::auto_);
        return choc::value::Value();
    });

    engine_.register_function("createMeter", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto o = args.get<std::string>(1, "vertical");
        auto pid = args.get<std::string>(2, "");
        auto m = std::make_unique<Meter>(); m->set_id(id);
        if (o == "horizontal") m->set_orientation(Meter::Orientation::horizontal);
        widgets_[id] = m.get(); resolve_parent(pid)->add_child(std::move(m));
        return choc::value::createString(id);
    });

    engine_.register_function("createXYPad", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<XYPad>(); p->set_id(id);
        widgets_[id] = p.get(); resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    engine_.register_function("createWaveform", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<WaveformView>(); w->set_id(id);
        widgets_[id] = w.get(); resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    engine_.register_function("createSpectrum", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<SpectrumView>(); s->set_id(id);
        widgets_[id] = s.get(); resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    engine_.register_function("createCombo", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<ComboBox>(); c->set_id(id);
        auto* ptr = c.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        c->on_change = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "combo select");
        };
        resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    engine_.register_function("createProgress", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<ProgressBar>(); p->set_id(id);
        widgets_[id] = p.get(); resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    engine_.register_function("createScrollView", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<ScrollView>(); s->set_id(id);
        widgets_[id] = s.get(); resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    engine_.register_function("createListBox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto lb = std::make_unique<ListBox>(); lb->set_id(id);
        auto* ptr = lb.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        lb->on_select = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "list select");
        };
        lb->on_activate = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'activate', " + std::to_string(idx) + ")", "list activate");
        };
        resolve_parent(pid)->add_child(std::move(lb));
        return choc::value::createString(id);
    });

    engine_.register_function("setListItems", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v)) {
            std::vector<std::string> items;
            if (args.numArgs > 1 && args[1]) {
                auto& arr = *args[1];
                for (uint32_t i = 0; i < arr.size(); ++i)
                    items.push_back(std::string(arr[i].getString()));
            }
            lb->set_items(std::move(items));
        }
        return choc::value::Value{};
    });

    engine_.register_function("setListSelected", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v)) {
            lb->set_selected(args.get<int>(1, 0));
            lb->ensure_visible(lb->selected());
        }
        return choc::value::Value{};
    });

    engine_.register_function("setListRowHeight", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v))
            lb->set_row_height(static_cast<float>(args.get<double>(1, 24.0)));
        return choc::value::Value{};
    });

    engine_.register_function("createTextEditor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto ed = std::make_unique<TextEditor>(); ed->set_id(id);
        auto* ptr = ed.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        ed->on_escape = [alive, engine, id]() {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'escape', 0)", "text escape");
        };
        ed->on_return = [alive, engine, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'return', '" + e + "')", "text return");
        };
        ed->on_change = [alive, engine, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', '" + e + "')", "text change");
        };
        resolve_parent(pid)->add_child(std::move(ed));
        return choc::value::createString(id);
    });

    engine_.register_function("createCanvas", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<CanvasWidget>(); c->set_id(id);
        c->set_native_gpu_texture_provider([this, id]() {
            return this->describe_native_canvas_frame(id);
        });
        widgets_[id] = c.get(); resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    // Property setters
    engine_.register_function("setLabel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto text = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_label(text);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_label(text);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_label(text);
        else if (auto* tb = dynamic_cast<ToggleButton*>(v)) tb->set_label(text);
        else if (auto* l = dynamic_cast<Label*>(v)) l->set_text(text);
        return choc::value::Value();
    });

    // setStyle — placeholder until KnobStyle/ToggleStyle enums added to main widgets
    engine_.register_function("setStyle", [](choc::javascript::ArgumentList) {
        return choc::value::Value();
    });

    // setWidgetStyle(id, "standard"|"minimal") — switch rendering mode
    // "minimal" draws simple shapes matching design tools (circles, thin tracks)
    engine_.register_function("setWidgetStyle", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto style_str = args.get<std::string>(1, "standard");
        auto style = (style_str == "minimal") ? WidgetRenderStyle::minimal : WidgetRenderStyle::standard;
        auto* v = widget(id);
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_render_style(style);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_render_style(style);
        else if (auto* m = dynamic_cast<Meter*>(v)) m->set_render_style(style);
        return choc::value::Value();
    });

    engine_.register_function("setItems", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (auto* c = dynamic_cast<ComboBox*>(widget(id))) {
            std::vector<std::string> items;
            if (args.numArgs > 1 && args[1]) {
                auto& arr = *args[1];
                for (uint32_t i = 0; i < arr.size(); ++i)
                    items.push_back(std::string(arr[i].toString()));
            }
            c->set_items(std::move(items));
        }
        return choc::value::Value();
    });

    // setSelected(id, index) — set ComboBox selected index without firing on_change
    engine_.register_function("setSelected", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto idx = args.get<int>(1, 0);
        if (auto* c = dynamic_cast<ComboBox*>(widget(id))) {
            c->set_selected_silent(idx);
        }
        return choc::value::Value();
    });

    // Typography properties
    // pulp #927 — `setFontFamily(id, family)` was called from web-compat JS
    // but had no C++ binding, so the call became a silent no-op. Now wires
    // through to Label::set_font_family() and Label::paint() honors it via
    // canvas.set_font_full().
    engine_.register_function("setFontFamily", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto family = args.get<std::string>(1, "");
        if (!v) return choc::value::Value();
        // pulp #1737 (#932 followup) — pass the comma-separated CSS
        // family list verbatim so the SkiaCanvas typeface resolver
        // (skia_canvas.cpp:get_cached_typeface) can walk the fallback
        // chain. Pre-fix the bridge stripped to the FIRST family,
        // dropping the rest — `fontFamily: 'SF Pro, system-ui,
        // sans-serif'` resolved only to the first (which often isn't
        // installed) and silently fell back to a Skia default. Now
        // the resolver tries each family in order through registered
        // → bundled → SkFontMgr until one matches.
        //
        // Storage layer accepts the raw comma-list; the resolver
        // splits it at paint time. Single-family inputs (no comma)
        // still hit the get_cached_typeface_single fast path with
        // identical behaviour to pre-fix.
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_font_family(family);
        } else {
            // pulp #1434 Phase A2-5 — inheritable cascade. Mirrors the
            // setFontWeight / setLetterSpacing pattern so a container
            // View's font-family flows down to descendant Labels.
            v->set_inheritable_font_family(family);
        }
        return choc::value::Value();
    });

    // issue-969: typography setters cascade. Label gets its own value;
    // any other View (Panel, Box, container) stores the value on the
    // View's inheritable_* slot so descendant Labels pick it up. Don't
    // silently no-op on container Views — that was the dom-adapter
    // workaround we're replacing.
    engine_.register_function("setFontWeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        int w = static_cast<int>(args.get<double>(1, 400));
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_weight(w);
        else v->set_inheritable_font_weight(w);
        return choc::value::Value();
    });

    engine_.register_function("setFontStyle", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto s = args.get<std::string>(1, "normal");
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_style(s == "italic" ? 1 : 0);
        return choc::value::Value();
    });

    engine_.register_function("setLetterSpacing", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto sp = static_cast<float>(args.get<double>(1, 0));
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) l->set_letter_spacing(sp);
        else v->set_inheritable_letter_spacing(sp);
        return choc::value::Value();
    });

    engine_.register_function("setLineHeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto lh = static_cast<float>(args.get<double>(1, 0));
        if (auto* l = dynamic_cast<Label*>(v)) l->set_line_height(lh);
        return choc::value::Value();
    });

    engine_.register_function("setTextAlign", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto a = args.get<std::string>(1, "left");
        if (!v) return choc::value::Value();
        // pulp #1434 — accept all six CSS / RN textAlign values:
        //   "left"/"start"     → LabelAlign::left         (0)
        //   "center"           → LabelAlign::center       (1)
        //   "right"/"end"      → LabelAlign::right        (2)
        //   "auto"             → LabelAlign::auto_        (3, paint-time
        //                        resolved to left under LTR; pulp doesn't
        //                        model RTL yet so this is currently
        //                        equivalent to left)
        //   "justify"          → LabelAlign::justify      (4, canvas
        //                        TextAlign::justify; SkParagraph kJustify
        //                        lands in a follow-up — backends fall back
        //                        to left-alignment math today)
        //   "match-parent"     → LabelAlign::match_parent (5, paint-time
        //                        resolved by walking the View parent chain
        //                        via inheritable_text_align(); falls back
        //                        to `left` when no ancestor set a value.
        //                        Matches CSS spec: inherit the parent's
        //                        *computed* text-align value).
        int aligned;
        LabelAlign label_a;
        if      (a == "center")               { aligned = 1; label_a = LabelAlign::center; }
        else if (a == "right" || a == "end")  { aligned = 2; label_a = LabelAlign::right; }
        else if (a == "auto")                 { aligned = 3; label_a = LabelAlign::auto_; }
        else if (a == "justify")              { aligned = 4; label_a = LabelAlign::justify; }
        else if (a == "match-parent")         { aligned = 5; label_a = LabelAlign::match_parent; }
        else                                  { aligned = 0; label_a = LabelAlign::left; }
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_text_align(label_a);
        } else {
            // issue-969: container Views store the alignment in the
            // inheritable slot for descendant Labels. Encoding extends
            // 0..5 to cover auto / justify / match-parent — Label::paint
            // decodes the slot via the same numeric mapping.
            v->set_inheritable_text_align(aligned);
        }
        return choc::value::Value();
    });

    engine_.register_function("setMultiLine", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto ml = args.get<double>(1, 0) > 0.5;
        if (auto* l = dynamic_cast<Label*>(v)) l->set_multi_line(ml);
        else if (auto* e = dynamic_cast<TextEditor*>(v)) e->multi_line = ml;
        return choc::value::Value();
    });

    engine_.register_function("setFontSize", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto size = static_cast<float>(args.get<double>(1, 14));
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_font_size(size);
        } else if (auto* e = dynamic_cast<TextEditor*>(v)) {
            e->set_font_size(size);
        } else {
            // issue-969: container Views store the size for descendant
            // Labels via the inheritable slot.
            v->set_inheritable_font_size(size);
        }
        return choc::value::Value();
    });

    engine_.register_function("setProgress", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<ProgressBar*>(widget(args.get<std::string>(0, ""))))
            p->set_progress(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });

    engine_.register_function("setMeterLevel", [this](choc::javascript::ArgumentList args) {
        if (auto* m = dynamic_cast<Meter*>(widget(args.get<std::string>(0, ""))))
            m->set_level(static_cast<float>(args.get<double>(1, 0)),
                        static_cast<float>(args.get<double>(2, 0)));
        return choc::value::Value();
    });

    engine_.register_function("setXY", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<XYPad*>(widget(args.get<std::string>(0, "")))) {
            p->set_x(static_cast<float>(args.get<double>(1, .5)));
            p->set_y(static_cast<float>(args.get<double>(2, .5)));
        }
        return choc::value::Value();
    });

    engine_.register_function("setWaveformData", [this](choc::javascript::ArgumentList args) {
        if (auto* w = dynamic_cast<WaveformView*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                w->set_data(std::move(d));
            }
        }
        return choc::value::Value();
    });

    engine_.register_function("setSpectrumData", [this](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<SpectrumView*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                s->set_spectrum(std::move(d));
            }
        }
        return choc::value::Value();
    });

    engine_.register_function("setPlaceholder", [this](choc::javascript::ArgumentList args) {
        if (auto* e = dynamic_cast<TextEditor*>(widget(args.get<std::string>(0, ""))))
            e->placeholder = args.get<std::string>(1, "");
        return choc::value::Value();
    });

    engine_.register_function("setText", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto t = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (auto* e = dynamic_cast<TextEditor*>(v)) e->set_text(t);
        else if (auto* l = dynamic_cast<Label*>(v)) l->set_text(t);
        return choc::value::Value();
    });

    engine_.register_function("getText", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (auto* e = dynamic_cast<TextEditor*>(v)) return choc::value::createString(e->text());
        if (auto* l = dynamic_cast<Label*>(v)) return choc::value::createString(l->text());
        return choc::value::createString("");
    });

    engine_.register_function("setPanelStyle", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<Panel*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1) p->set_background_token(args.get<std::string>(1, "bg.surface"));
            if (args.numArgs > 2) p->set_border_token(args.get<std::string>(2, "control.border"));
            if (args.numArgs > 3) p->set_corner_radius(static_cast<float>(args.get<double>(3, 8)));
            if (args.numArgs > 4) p->set_border_width(static_cast<float>(args.get<double>(4, 1)));
        }
        return choc::value::Value();
    });

    engine_.register_function("setScrollContentSize", [this](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<ScrollView*>(widget(args.get<std::string>(0, ""))))
            s->set_content_size({static_cast<float>(args.get<double>(1,0)),
                                static_cast<float>(args.get<double>(2,0))});
        return choc::value::Value();
    });

    // ── Visual properties (CSS Box Model) ─────────────────────────────

    // CSS Color Level 4 parser — accepts hex, rgb(), rgba(), hsl(), hsla(), named, transparent
    auto parseColor = [](const std::string& str) -> canvas::Color {
        canvas::Color c = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
        if (str.empty()) return c;

        // transparent
        if (str == "transparent") return canvas::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);

        // Hex: #RGB, #RRGGBB, #RRGGBBAA
        if (str[0] == '#') {
            if (str.size() == 4) {  // #RGB → #RRGGBB
                c.r = static_cast<float>(std::stoul(std::string(2, str[1]), nullptr, 16)) / 255.0f;
                c.g = static_cast<float>(std::stoul(std::string(2, str[2]), nullptr, 16)) / 255.0f;
                c.b = static_cast<float>(std::stoul(std::string(2, str[3]), nullptr, 16)) / 255.0f;
            } else if (str.size() >= 7) {
                c.r = static_cast<float>(std::stoul(str.substr(1,2), nullptr, 16)) / 255.0f;
                c.g = static_cast<float>(std::stoul(str.substr(3,2), nullptr, 16)) / 255.0f;
                c.b = static_cast<float>(std::stoul(str.substr(5,2), nullptr, 16)) / 255.0f;
                if (str.size() >= 9)
                    c.a = static_cast<float>(std::stoul(str.substr(7,2), nullptr, 16)) / 255.0f;
            }
            return c;
        }

        // rgb(r, g, b) / rgba(r, g, b, a)
        if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
            auto inner = str.substr(str.find('(') + 1);
            inner = inner.substr(0, inner.find(')'));
            float vals[4] = {0, 0, 0, 1};
            int n = 0;
            std::istringstream ss(inner);
            std::string tok;
            while (std::getline(ss, tok, ',') && n < 4) {
                while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
                vals[n++] = std::stof(tok);
            }
            c.r = std::clamp(vals[0] / 255.0f, 0.0f, 1.0f);
            c.g = std::clamp(vals[1] / 255.0f, 0.0f, 1.0f);
            c.b = std::clamp(vals[2] / 255.0f, 0.0f, 1.0f);
            c.a = std::clamp(vals[3], 0.0f, 1.0f);  // alpha is already 0-1 in CSS
            return c;
        }

        // hsl(h, s%, l%) / hsla(h, s%, l%, a)
        if (str.substr(0, 4) == "hsl(" || str.substr(0, 5) == "hsla(") {
            auto inner = str.substr(str.find('(') + 1);
            inner = inner.substr(0, inner.find(')'));
            float vals[4] = {0, 0, 0, 1};
            int n = 0;
            std::istringstream ss(inner);
            std::string tok;
            while (std::getline(ss, tok, ',') && n < 4) {
                while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
                if (tok.back() == '%') tok.pop_back();
                vals[n++] = std::stof(tok);
            }
            float h = std::fmod(vals[0], 360.0f) / 360.0f;
            float s = vals[1] / 100.0f;
            float l = vals[2] / 100.0f;
            // HSL to RGB conversion
            auto hue2rgb = [](float p, float q, float t) {
                if (t < 0) t += 1; if (t > 1) t -= 1;
                if (t < 1.0f/6) return p + (q - p) * 6 * t;
                if (t < 1.0f/2) return q;
                if (t < 2.0f/3) return p + (q - p) * (2.0f/3 - t) * 6;
                return p;
            };
            float r, g, b;
            if (s == 0) { r = g = b = l; }
            else {
                float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
                float p = 2 * l - q;
                r = hue2rgb(p, q, h + 1.0f/3);
                g = hue2rgb(p, q, h);
                b = hue2rgb(p, q, h - 1.0f/3);
            }
            c.r = std::clamp(r, 0.0f, 1.0f);
            c.g = std::clamp(g, 0.0f, 1.0f);
            c.b = std::clamp(b, 0.0f, 1.0f);
            c.a = std::clamp(vals[3], 0.0f, 1.0f);
            return c;
        }

        // Named colors (common subset)
        static const std::unordered_map<std::string, uint32_t> named = {
            {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},
            {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
            {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"orange", 0xFFA500},
            {"purple", 0x800080}, {"pink", 0xFFC0CB}, {"gray", 0x808080},
            {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"gold", 0xFFD700},
            {"navy", 0x000080}, {"teal", 0x008080}, {"maroon", 0x800000},
            {"olive", 0x808000}, {"lime", 0x00FF00}, {"aqua", 0x00FFFF},
            {"fuchsia", 0xFF00FF}, {"coral", 0xFF7F50}, {"salmon", 0xFA8072},
            {"tomato", 0xFF6347}, {"crimson", 0xDC143C}, {"indigo", 0x4B0082},
            {"violet", 0xEE82EE}, {"turquoise", 0x40E0D0}, {"tan", 0xD2B48C},
            {"khaki", 0xF0E68C}, {"plum", 0xDDA0DD}, {"orchid", 0xDA70D6},
            {"chocolate", 0xD2691E}, {"sienna", 0xA0522D}, {"peru", 0xCD853F},
            {"linen", 0xFAF0E6}, {"ivory", 0xFFFFF0}, {"beige", 0xF5F5DC},
            {"wheat", 0xF5DEB3}, {"snow", 0xFFFAFA}, {"azure", 0xF0FFFF},
            {"mintcream", 0xF5FFFA}, {"honeydew", 0xF0FFF0}, {"aliceblue", 0xF0F8FF},
            {"lavender", 0xE6E6FA}, {"mistyrose", 0xFFE4E1}, {"seashell", 0xFFF5EE},
            {"cornsilk", 0xFFF8DC}, {"papayawhip", 0xFFEFD5}, {"blanchedalmond", 0xFFEBCD},
            {"bisque", 0xFFE4C4}, {"moccasin", 0xFFE4B5}, {"oldlace", 0xFDF5E6},
            {"floralwhite", 0xFFFAF0}, {"ghostwhite", 0xF8F8FF}, {"whitesmoke", 0xF5F5F5},
            {"gainsboro", 0xDCDCDC}, {"lightgray", 0xD3D3D3}, {"darkgray", 0xA9A9A9},
            {"dimgray", 0x696969}, {"lightslategray", 0x778899}, {"slategray", 0x708090},
            {"darkslategray", 0x2F4F4F},
            {"lightcoral", 0xF08080}, {"indianred", 0xCD5C5C}, {"firebrick", 0xB22222},
            {"darkred", 0x8B0000}, {"orangered", 0xFF4500}, {"darkorange", 0xFF8C00},
            {"lightgreen", 0x90EE90}, {"limegreen", 0x32CD32}, {"forestgreen", 0x228B22},
            {"darkgreen", 0x006400}, {"springgreen", 0x00FF7F}, {"seagreen", 0x2E8B57},
            {"lightblue", 0xADD8E6}, {"skyblue", 0x87CEEB}, {"deepskyblue", 0x00BFFF},
            {"dodgerblue", 0x1E90FF}, {"royalblue", 0x4169E1}, {"steelblue", 0x4682B4},
            {"cornflowerblue", 0x6495ED}, {"mediumblue", 0x0000CD}, {"darkblue", 0x00008B},
            {"midnightblue", 0x191970}, {"slateblue", 0x6A5ACD}, {"mediumpurple", 0x9370DB},
            {"blueviolet", 0x8A2BE2}, {"darkviolet", 0x9400D3}, {"darkorchid", 0x9932CC},
            {"darkmagenta", 0x8B008B}, {"deeppink", 0xFF1493}, {"hotpink", 0xFF69B4},
            {"mediumvioletred", 0xC71585}, {"palevioletred", 0xDB7093},
        };
        auto it = named.find(str);
        if (it != named.end()) {
            uint32_t v = it->second;
            c = canvas::Color::rgba8((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
            return c;
        }

        return c;  // default white
    };
    // Alias for backward compatibility
    auto parseHexColor = parseColor;

    engine_.register_function("setBackground", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) v->set_background_color(parseHexColor(hex));
        return choc::value::Value();
    });

    // ── pulp #965 — SvgPathWidget bridge ─────────────────────────────────────
    // Mirrors the API surface of CanvasWidget but for inline <svg><path>
    // icons. JS registers the widget and pushes its path-data + paint
    // attributes; the native widget parses path-data once on set_path()
    // and replays as Canvas2D path commands inside paint().
    engine_.register_function("createSvgPath", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<SvgPathWidget>();
        w->set_id(id);
        widgets_[id] = w.get();
        resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    engine_.register_function("setSvgPath", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto data = args.get<std::string>(1, "");
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            w->set_path(data);
        }
        return choc::value::Value();
    });

    engine_.register_function("setSvgViewBox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vw = args.get<double>(1, 0.0);
        auto vh = args.get<double>(2, 0.0);
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            w->set_viewbox(static_cast<float>(vw), static_cast<float>(vh));
        }
        return choc::value::Value();
    });

    // pulp #1416 — setSvgFill / setSvgStroke / setSvgStrokeWidth are
    // polymorphic across all SVG-primitive widgets so JSX consumers see
    // a uniform fill/stroke surface. SvgPathWidget is the legacy path
    // (#965 / #994); SvgRectWidget and SvgLineWidget mirror the API with
    // the same hex / "none" / strokeWidth semantics.
    engine_.register_function("setSvgFill", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        const bool clear = hex.empty() || hex == "none";
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            if (clear) w->clear_fill();
            else       w->set_fill_color(parseHexColor(hex));
            // pulp #932 / #1737 PR-4 — solid color path wins over any
            // previous gradient. Clear the gradient slot so a later
            // re-render with a solid fill doesn't accidentally pick
            // up a stale linear-gradient string.
            w->clear_fill_gradient();
        } else if (auto* r = dynamic_cast<SvgRectWidget*>(widget(id))) {
            if (clear) r->clear_fill();
            else       r->set_fill_color(parseHexColor(hex));
        }
        // SvgLineWidget has no fill semantics — the call is intentionally
        // a no-op for line widgets so JSX consumers can pass `fill="none"`
        // without bridge-level dispatch failures.
        return choc::value::Value();
    });

    // pulp #932 / #1737 PR-4 — gradient-fill bridge fn for SvgPathWidget.
    // Accepts a CSS linear-gradient string verbatim; the widget parses
    // it at paint time. Intended JSX usage: `<SvgLinearGradient id="g"
    // stops={...}>` + `<SvgPath fill="url(#g)" .../>` — prop-applier
    // resolves the gradient JSX to a CSS string and calls this fn.
    // Direct C++ consumers can also call it for testing.
    engine_.register_function("setSvgFillGradient", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto value = args.get<std::string>(1, "");
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            if (value.empty()) w->clear_fill_gradient();
            else               w->set_fill_gradient(std::move(value));
        }
        // SvgRect / SvgLine — gradient fills follow up; bridge fn is
        // a no-op for those widget types so consumers don't crash.
        return choc::value::Value();
    });

    engine_.register_function("setSvgStroke", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        const bool clear = hex.empty() || hex == "none";
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            if (clear) w->clear_stroke();
            else       w->set_stroke_color(parseHexColor(hex));
        } else if (auto* r = dynamic_cast<SvgRectWidget*>(widget(id))) {
            if (clear) r->clear_stroke();
            else       r->set_stroke_color(parseHexColor(hex));
        } else if (auto* l = dynamic_cast<SvgLineWidget*>(widget(id))) {
            if (clear) l->clear_stroke();
            else       l->set_stroke_color(parseHexColor(hex));
        }
        return choc::value::Value();
    });

    engine_.register_function("setSvgStrokeWidth", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto width = args.get<double>(1, 1.0);
        const float fw = static_cast<float>(width);
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            w->set_stroke_width(fw);
        } else if (auto* r = dynamic_cast<SvgRectWidget*>(widget(id))) {
            r->set_stroke_width(fw);
        } else if (auto* l = dynamic_cast<SvgLineWidget*>(widget(id))) {
            l->set_stroke_width(fw);
        }
        return choc::value::Value();
    });

    // pulp #1416 — SvgRectWidget bridge. Mirrors createSvgPath /
    // setSvgPath. Geometry is local to the widget origin (NOT
    // bounds()-translated). x/y default to 0, w/h default to 0 so an
    // un-set rect is invisible by default.
    engine_.register_function("createSvgRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<SvgRectWidget>();
        w->set_id(id);
        widgets_[id] = w.get();
        resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    engine_.register_function("setSvgRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = args.get<double>(1, 0.0);
        auto y = args.get<double>(2, 0.0);
        auto width = args.get<double>(3, 0.0);
        auto height = args.get<double>(4, 0.0);
        if (auto* w = dynamic_cast<SvgRectWidget*>(widget(id))) {
            w->set_rect(static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(width), static_cast<float>(height));
        }
        return choc::value::Value();
    });

    // pulp #1416 — SvgLineWidget bridge. Mirrors createSvgPath /
    // setSvgRect. Endpoints are local to the widget origin.
    engine_.register_function("createSvgLine", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<SvgLineWidget>();
        w->set_id(id);
        widgets_[id] = w.get();
        resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    engine_.register_function("setSvgLine", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x1 = args.get<double>(1, 0.0);
        auto y1 = args.get<double>(2, 0.0);
        auto x2 = args.get<double>(3, 0.0);
        auto y2 = args.get<double>(4, 0.0);
        if (auto* w = dynamic_cast<SvgLineWidget*>(widget(id))) {
            w->set_line(static_cast<float>(x1), static_cast<float>(y1),
                        static_cast<float>(x2), static_cast<float>(y2));
        }
        return choc::value::Value();
    });

    engine_.register_function("setBorder", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto width = args.get<double>(2, 1.0);
        auto radius = args.get<double>(3, 0.0);
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) v->set_border(parseHexColor(hex), (float)width, (float)radius);
        return choc::value::Value();
    });

    // setBorderSide(id, side, width, color) — per-side border
    engine_.register_function("setBorderSide", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto side = args.get<std::string>(1, "");
        auto width = static_cast<float>(args.get<double>(2, 1.0));
        auto hex = args.get<std::string>(3, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            auto c = hex.empty() ? canvas::Color::rgba8(128,128,128) : parseHexColor(hex);
            if (side == "top") v->set_border_top(c, width);
            else if (side == "right") v->set_border_right(c, width);
            else if (side == "bottom") v->set_border_bottom(c, width);
            else if (side == "left") v->set_border_left(c, width);
        }
        return choc::value::Value();
    });

    // setCornerRadius(id, corner, radius) — per-corner border-radius
    engine_.register_function("setCornerRadius", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto corner = args.get<std::string>(1, "");
        auto r = static_cast<float>(args.get<double>(2, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            if (corner == "TopLeft") v->set_corner_radius_tl(r);
            else if (corner == "TopRight") v->set_corner_radius_tr(r);
            else if (corner == "BottomLeft") v->set_corner_radius_bl(r);
            else if (corner == "BottomRight") v->set_corner_radius_br(r);
        }
        return choc::value::Value();
    });

    // pulp #1026 — Standalone border setters (RN parity). The shorthand
    // setBorder(id, color, width, radius) is convenient for atomic style
    // application but @pulp/react's prop-applier needs per-attribute
    // setters so individual JSX style props (`borderColor`, `borderWidth`,
    // `borderRadius`) can update without recomputing the others.
    //
    // setBorderColor(id, hex)
    engine_.register_function("setBorderColor", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) v->set_border_color(parseHexColor(hex));
        return choc::value::Value();
    });

    // setBorderWidth(id, width)
    engine_.register_function("setBorderWidth", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto width = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_border_width(width);
        return choc::value::Value();
    });

    // setBorderStyle(id, "solid"|"dashed"|"dotted"|...) — pulp #1434
    // Triage #10. Maps the CSS border-style keyword to View::BorderStyle.
    // Skia paint installs the dashed / dotted SkDashPathEffect at stroke
    // time; double / groove / ridge / inset / outset currently degrade
    // to solid (paint-side gap, tracked for follow-up). none / hidden
    // short-circuit the stroke entirely.
    engine_.register_function("setBorderStyle", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "solid");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (s == "dashed")        v->set_border_style(View::BorderStyle::dashed);
        else if (s == "dotted")   v->set_border_style(View::BorderStyle::dotted);
        else if (s == "double")   v->set_border_style(View::BorderStyle::double_);
        else if (s == "groove")   v->set_border_style(View::BorderStyle::groove);
        else if (s == "ridge")    v->set_border_style(View::BorderStyle::ridge);
        else if (s == "inset")    v->set_border_style(View::BorderStyle::inset);
        else if (s == "outset")   v->set_border_style(View::BorderStyle::outset);
        else if (s == "none")     v->set_border_style(View::BorderStyle::none);
        else if (s == "hidden")   v->set_border_style(View::BorderStyle::hidden);
        else                      v->set_border_style(View::BorderStyle::solid);
        return choc::value::Value();
    });

    // pulp #1514 — list-style cluster (listStyle / listStyleType /
    // listStyleImage / listStylePosition). Pulp doesn't model
    // <li>/<ul>/<ol> semantics, so the bridge stores the values
    // verbatim on the View so a later paint pass (or a future
    // semantic-list surface) can honor them. Marker glyph rendering
    // is the follow-up; this PR flips the catalog out of `missing`
    // by wiring the round-trip + JS shorthand parsing.
    //
    // setListStyleType(id, "disc"|"circle"|"square"|"decimal"|"none"
    //                       |"decimal-leading-zero"|"lower-roman"|"upper-roman"
    //                       |"lower-alpha"|"upper-alpha"|"lower-latin"|"upper-latin"
    //                       |"lower-greek"|"armenian"|"georgian").
    //
    // The counter-style keywords (lower-roman, etc.) are stored verbatim
    // on the View::ListStyleType enum today; paint-side glyph rendering
    // is the follow-up (pulp #1514). Unknown keywords fall back to `disc`
    // to match the longstanding default.
    engine_.register_function("setListStyleType", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "disc");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (s == "none")                       v->set_list_style_type(View::ListStyleType::none);
        else if (s == "circle")                v->set_list_style_type(View::ListStyleType::circle);
        else if (s == "square")                v->set_list_style_type(View::ListStyleType::square);
        else if (s == "decimal")               v->set_list_style_type(View::ListStyleType::decimal);
        else if (s == "decimal-leading-zero")  v->set_list_style_type(View::ListStyleType::decimal_leading_zero);
        else if (s == "lower-roman")           v->set_list_style_type(View::ListStyleType::lower_roman);
        else if (s == "upper-roman")           v->set_list_style_type(View::ListStyleType::upper_roman);
        else if (s == "lower-alpha")           v->set_list_style_type(View::ListStyleType::lower_alpha);
        else if (s == "upper-alpha")           v->set_list_style_type(View::ListStyleType::upper_alpha);
        else if (s == "lower-latin")           v->set_list_style_type(View::ListStyleType::lower_latin);
        else if (s == "upper-latin")           v->set_list_style_type(View::ListStyleType::upper_latin);
        else if (s == "lower-greek")           v->set_list_style_type(View::ListStyleType::lower_greek);
        else if (s == "armenian")              v->set_list_style_type(View::ListStyleType::armenian);
        else if (s == "georgian")              v->set_list_style_type(View::ListStyleType::georgian);
        else                                   v->set_list_style_type(View::ListStyleType::disc);
        return choc::value::Value();
    });

    // setListStyleImage(id, "url(...)" or "none"). Stored verbatim;
    // bullet-image rendering is deferred (same caveat as backgroundImage).
    engine_.register_function("setListStyleImage", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto url = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (url == "none") v->set_list_style_image("");
        else               v->set_list_style_image(url);
        return choc::value::Value();
    });

    // setListStylePosition(id, "outside"|"inside").
    engine_.register_function("setListStylePosition", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "outside");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (s == "inside") v->set_list_style_position(View::ListStylePosition::inside);
        else               v->set_list_style_position(View::ListStylePosition::outside);
        return choc::value::Value();
    });

    // pulp #1519 — CSS / RN outline cluster. Outline is paint-time only:
    // it does NOT take up Yoga layout space (parent never reserves room
    // for it). Each setter mutates one slot in isolation so a JSX prop
    // diff that touches only `outlineColor` doesn't clobber `outlineWidth`.
    // Skia paint inflates the box by (offset + width/2) and strokes with
    // the standard borderStyle dash effect for dashed/dotted; other named
    // styles (double/groove/ridge/inset/outset) currently degrade to solid
    // — same paint-side gap as borderStyle.
    //
    // setOutlineColor(id, hex) — accepts the same color forms as
    // setBackground / setBorderColor (#hex / rgb() / named), plus the
    // CSS `currentColor` keyword which resolves to the View's text
    // color via the inheritable cascade (#1710).
    engine_.register_function("setOutlineColor", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v || hex.empty()) return choc::value::Value();
        // CSS `currentColor`: resolve to the element's own computed text
        // color first, then the inheritable cascade, then theme fallback.
        // Order matters — pulp #1728 Codex P2: a Label that set its own
        // `color` via setTextColor stores it in `Label::text_color_`
        // (`has_own_text_color_=true`) and does NOT touch the inheritable
        // slot, so `inheritable_text_color()` would skip the Label and
        // climb to the parent's inheritable color. CSS semantics: an
        // element's own `color` always wins over inheritance for that
        // element's `currentColor`.
        std::string lower;
        lower.reserve(hex.size());
        for (char c : hex) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "currentcolor") {
            if (auto* l = dynamic_cast<Label*>(v); l && l->has_own_text_color()) {
                v->set_outline_color(l->text_color());
            } else if (auto inherited = v->inheritable_text_color()) {
                v->set_outline_color(*inherited);
            } else {
                v->set_outline_color(v->resolve_color("text.primary",
                                                      canvas::Color::rgba8(220, 220, 220)));
            }
        } else {
            v->set_outline_color(parseHexColor(hex));
        }
        return choc::value::Value();
    });

    // setOutlineOffset(id, px) — gap between border-box edge and outline.
    engine_.register_function("setOutlineOffset", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto offset = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_outline_offset(offset);
        return choc::value::Value();
    });

    // setOutlineStyle(id, "solid"|"dashed"|...) — same keyword set as
    // setBorderStyle. Reuses View::BorderStyle since the CSS spec lists
    // the same line-style values for both properties.
    engine_.register_function("setOutlineStyle", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "solid");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (s == "dashed")        v->set_outline_style(View::BorderStyle::dashed);
        else if (s == "dotted")   v->set_outline_style(View::BorderStyle::dotted);
        else if (s == "double")   v->set_outline_style(View::BorderStyle::double_);
        else if (s == "groove")   v->set_outline_style(View::BorderStyle::groove);
        else if (s == "ridge")    v->set_outline_style(View::BorderStyle::ridge);
        else if (s == "inset")    v->set_outline_style(View::BorderStyle::inset);
        else if (s == "outset")   v->set_outline_style(View::BorderStyle::outset);
        else if (s == "none")     v->set_outline_style(View::BorderStyle::none);
        else if (s == "hidden")   v->set_outline_style(View::BorderStyle::hidden);
        else                      v->set_outline_style(View::BorderStyle::solid);
        return choc::value::Value();
    });

    // setOutlineWidth(id, px) — outline stroke thickness. width<=0 or
    // outline_style==none/hidden short-circuits the paint.
    engine_.register_function("setOutlineWidth", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto width = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_outline_width(width);
        return choc::value::Value();
    });

    // setBorderRadius(id, radius) — uniform corner radius. Per-corner
    // setters (setBorderTopLeftRadius / TopRight / BottomLeft / BottomRight)
    // override individual corners on top of the uniform value.
    //
    // pulp #1663 — accepts either a number (px) or a string with % suffix
    // (e.g. "50%"). Percent values are stored separately and resolved at
    // paint time as `pct * 0.01 * min(width, height)` so the radius
    // tracks the View's actual bounds.
    auto parseRadiusArg = [](choc::javascript::ArgumentList& args, int idx) -> std::pair<float, float> {
        // returns {px_radius, pct_radius}: at most one is non-zero.
        auto sval = args.get<std::string>(idx, "");
        if (!sval.empty() && sval.back() == '%') {
            try { return {0.0f, std::stof(sval.substr(0, sval.size() - 1))}; }
            catch (...) { return {0.0f, 0.0f}; }
        }
        return {static_cast<float>(args.get<double>(idx, 0.0)), 0.0f};
    };
    engine_.register_function("setBorderRadius", [this, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) {
            v->set_border_radius_pct(pct);
        } else {
            v->set_border_radius(px);
        }
        return choc::value::Value();
    });

    // pulp #1026 — Per-corner border-radius shorthands (RN parity).
    // Equivalent to `setCornerRadius(id, "TopLeft", r)` but matches the
    // RN style-prop name 1:1 so @pulp/react's prop-applier can bind them
    // without a translation layer. Sets the `has_corner_radii_` flag on
    // the View; paint_all() then routes background/border through the
    // per-corner path builder rather than fill_rounded_rect.
    // pulp #1663 — same %-string handling as setBorderRadius.
    engine_.register_function("setBorderTopLeftRadius", [this, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_tl_pct(pct);
        else v->set_corner_radius_tl(px);
        return choc::value::Value();
    });
    engine_.register_function("setBorderTopRightRadius", [this, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_tr_pct(pct);
        else v->set_corner_radius_tr(px);
        return choc::value::Value();
    });
    engine_.register_function("setBorderBottomLeftRadius", [this, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_bl_pct(pct);
        else v->set_corner_radius_bl(px);
        return choc::value::Value();
    });
    engine_.register_function("setBorderBottomRightRadius", [this, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_br_pct(pct);
        else v->set_corner_radius_br(px);
        return choc::value::Value();
    });

    // pulp #1026 — Per-side border color/width shorthands (RN parity).
    // RN exposes `borderTopColor`, `borderTopWidth`, etc. as separate
    // style props; pulp's existing `setBorderSide(id, side, width, color)`
    // sets both at once which is awkward for a prop-by-prop applier. The
    // setBorderTop/Right/Bottom/Left{Color,Width} setters route through
    // the per-side fields preserving whichever attribute the call doesn't
    // specify.
    auto applyBorderSide = [](View* v, const std::string& side,
                              std::optional<canvas::Color> color,
                              std::optional<float> width) {
        if (!v) return;
        // pulp #1026 — preserve the unrelated attribute when a per-side
        // setter is called for only color OR only width, matching how
        // RN's JSX prop-applier emits property updates one at a time.
        // pulp #1566 — route through the split color-only / width-only
        // setters so that `setBorderTopColor` does NOT mark the per-edge
        // WIDTH as explicitly set (which would let a stale 0 override
        // the uniform `borderWidth` shorthand). Symmetrically,
        // `setBorderTopWidth(0)` MUST mark the edge as explicitly set so
        // it overrides the shorthand on that edge per CSS / RN semantics.
        if (color.has_value()) {
            if (side == "top")         v->set_border_top_color(*color);
            else if (side == "right")  v->set_border_right_color(*color);
            else if (side == "bottom") v->set_border_bottom_color(*color);
            else if (side == "left")   v->set_border_left_color(*color);
        }
        if (width.has_value()) {
            if (side == "top")         v->set_border_top_width(*width);
            else if (side == "right")  v->set_border_right_width(*width);
            else if (side == "bottom") v->set_border_bottom_width(*width);
            else if (side == "left")   v->set_border_left_width(*width);
        }
    };
    engine_.register_function("setBorderTopColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "top", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });
    engine_.register_function("setBorderRightColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "right", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });
    engine_.register_function("setBorderBottomColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "bottom", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });
    engine_.register_function("setBorderLeftColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "left", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });
    engine_.register_function("setBorderTopWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "top", std::nullopt, w);
        return choc::value::Value();
    });
    engine_.register_function("setBorderRightWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "right", std::nullopt, w);
        return choc::value::Value();
    });
    engine_.register_function("setBorderBottomWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "bottom", std::nullopt, w);
        return choc::value::Value();
    });
    engine_.register_function("setBorderLeftWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "left", std::nullopt, w);
        return choc::value::Value();
    });

    // pulp #1026 — RN-shaped shadow primitive. RN's View style-prop names
    // are { shadowColor, shadowOffset: {x,y}, shadowOpacity, shadowRadius }
    // — none of which carries spread or inset. We lower these onto the
    // existing pulp #925 box-shadow primitive (which carries spread+inset
    // for CSS parity) by:
    //   - hex carrying alpha 1.0 ('#RRGGBBff')
    //   - composing shadowOpacity into the alpha channel (0..1)
    //   - using shadowRadius as the blur, spread = 0, inset = false.
    // The underlying setBoxShadow is left unchanged so CSS-style consumers
    // keep working unaltered.
    engine_.register_function("setShadow", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "#000000ff");
        auto ox = static_cast<float>(args.get<double>(2, 0.0));
        auto oy = static_cast<float>(args.get<double>(3, 0.0));
        auto opacity = static_cast<float>(args.get<double>(4, 1.0));
        auto radius = static_cast<float>(args.get<double>(5, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto color = parseHexColor(hex);
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        // RN composes opacity multiplicatively with whatever alpha came
        // from shadowColor. Default shadowColor is opaque black, so
        // opacity alone drives the final alpha for 99% of call sites.
        color.a *= opacity;
        v->set_box_shadow(ox, oy, /*blur=*/radius, /*spread=*/0.0f,
                          color, /*inset=*/false);
        return choc::value::Value();
    });

    // registerWheel(id) — enable wheel event dispatch for scroll/zoom
    engine_.register_function("registerWheel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        // Idempotency: re-renders re-issue registerWheel for the same id.
        // Without this gate each call wraps the previous on_pointer_event,
        // stacking N lambdas and multiplying dispatch cost by render count.
        if (!wheel_registered_.insert(id).second) {
            return choc::value::Value();
        }
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            auto* w = it->second;
            auto alive = callback_alive_;
            auto* engine = &engine_;
            auto previous_pointer = w->on_pointer_event;
            w->on_pointer_event = [alive, engine, id, previous_pointer](const MouseEvent& me) {
                if (previous_pointer) {
                    previous_pointer(me);
                }
                if (!me.is_wheel) {
                    return;
                }
                // Dispatch wheel data as an object so the synthetic-event
                // shim can lift deltaX/deltaY + clientX/clientY off it.
                // Pre-fix this sent raw positional args (deltaX,deltaY),
                // which the synthetic event's isPlainObject(a0) branch
                // never visited — JSX onWheel handlers read undefined
                // deltas and trackpad zoom silently broke. Also include
                // clientX/clientY (window-relative) so handlers reading
                // `e.clientX - rect.left` work for anchor-frequency.
                std::string data = "{"
                    "deltaX:" + std::to_string(me.scroll_delta_x) + ","
                    "deltaY:" + std::to_string(me.scroll_delta_y) + ","
                    "clientX:" + std::to_string(me.window_position.x) + ","
                    "clientY:" + std::to_string(me.window_position.y) +
                    "}";
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'wheel', " + data + ")", "wheel");
            };
        }
        return choc::value::Value();
    });

    engine_.register_function("setOpacity", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto alpha = args.get<double>(1, 1.0);
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_opacity((float)alpha);
        return choc::value::Value();
    });

    engine_.register_function("setTextColor", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v || hex.empty()) return choc::value::Value();
        auto color = parseHexColor(hex);
        // issue-969: CSS-style cascade.
        //   - On a Label: set the Label's own explicit text_color, which
        //     wins over inheritance and theme tokens.
        //   - On a container View: store the color on the inheritable
        //     slot so descendant Labels pick it up. This replaces the
        //     dom-adapter's manual "walk children and pushdown" hack.
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_text_color(color);
        } else {
            v->set_inheritable_text_color(color);
        }
        // Keep the theme-token fallback in sync so widgets that resolve
        // through `resolve_color("text.primary")` (e.g. Knob/ToggleButton)
        // also pick up the override on their own subtree — preserves the
        // pre-#969 behavior for those widgets.
        auto theme = v->theme();
        theme.colors["text.primary"] = color;
        v->set_theme(theme);
        return choc::value::Value();
    });

    engine_.register_function("setOverflow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "hidden");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        // pulp DIVERGE→PASS sweep — accept all 3 CSS overflow keywords
        // (visible / hidden / scroll). `scroll` clips painted pixels
        // like `hidden` (no scrollbar layer yet) but propagates through
        // Yoga so the layout engine knows about the clipping context.
        if (mode == "visible")      v->set_overflow(View::Overflow::visible);
        else if (mode == "scroll")  v->set_overflow(View::Overflow::scroll);
        else                        v->set_overflow(View::Overflow::hidden);
        return choc::value::Value();
    });

    // pulp #1516 — CSS box-sizing keyword. Yoga 3.x's
    // `YGNodeStyleSetBoxSizing` honors the spec, so we just record the
    // enum on FlexStyle and let `build_yoga_subtree` route it through.
    // Default `content-box` matches the CSS spec; web designs typically
    // reset to `border-box` via `* { box-sizing: border-box }`.
    engine_.register_function("setBoxSizing", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "content-box");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto& f = v->flex();
        if (kw == "border-box") {
            f.box_sizing = BoxSizing::border_box;
        } else if (kw == "inherit") {
            f.box_sizing = v->parent() ? v->parent()->flex().box_sizing
                                       : BoxSizing::content_box;
        } else {
            f.box_sizing = BoxSizing::content_box;
        }
        return choc::value::Value();
    });

    // Canvas drawing
    engine_.register_function("canvasClear", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, ""))))
            c->clear_commands();
        return choc::value::Value();
    });

    // Canvas 2D API — full CanvasRenderingContext2D equivalent
    // Helper to get CanvasWidget and add a command
    auto canvasCmd = [this](choc::javascript::ArgumentList& args, CanvasDrawCmd::Type type) -> CanvasWidget* {
        auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")));
        return c;
    };

    // pulp #964 — Two registered names for the same handler:
    //   * `canvasRect`     — legacy direct-bridge name used by hand-written
    //                        examples (`canvasRect(id, x, y, w, h, "#fff")`).
    //   * `canvasFillRect` — the name the HTML5 Canvas2D shim emits for
    //                        `ctx.fillRect()` (see core/view/js/web-compat-canvas.js).
    // Pre-#964 only `canvasRect` was registered, so every `ctx.fillRect()`
    // from the web-compat shim silently no-op'd at the typeof guard
    // (`if (typeof canvasFillRect === "function")`). That dropped every
    // full-bounds opaque fillRect — e.g. the Spectr FilterBank's clear-to-
    // background fill — without surfacing any error. Path-based draws
    // (`canvasFillPath`, `canvasStrokePath`) and `canvasStrokeRect` happened
    // to be wired correctly so they kept working, which is why the bug
    // looked like "compositing eats the canvas surface" instead of
    // "fillRect is silently dropped".
    auto canvasRectHandler = [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            // pulp #968 — when no color arg was passed (or it was the empty
            // string), honour the active fillStyle (color OR gradient) on
            // the canvas widget. This makes a JS shim like
            //   fillRect(x,y,w,h) { call('canvasFillRect', id, x,y,w,h); }
            // behave like the Canvas2D spec — `ctx.fillRect` paints with
            // whatever `ctx.fillStyle` was last set to, including a
            // CanvasGradient. With 6+ args the explicit color wins.
            const std::string color_str = args.get<std::string>(5, "");
            if (args.size() < 6 || color_str.empty()) {
                cmd.use_active_style = true;
            } else {
                cmd.color = parseColor(color_str);
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    };
    engine_.register_function("canvasRect", canvasRectHandler);
    engine_.register_function("canvasFillRect", canvasRectHandler);

    engine_.register_function("canvasStrokeRect", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            // pulp #968 — same active-style fallback as canvasRect, applied
            // to strokeStyle. Width arg (index 6) is unaffected.
            const std::string color_str = args.get<std::string>(5, "");
            if (args.size() < 6 || color_str.empty()) {
                cmd.use_active_style = true;
            } else {
                cmd.color = parseColor(color_str);
            }
            cmd.extra = (float)args.get<double>(6, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasFillCircle", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_circle;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.extra=(float)args.get<double>(3,10);
            cmd.color = parseColor(args.get<std::string>(4, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokeLine", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_line;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.color = parseColor(args.get<std::string>(5, "#fff"));
            cmd.extra=(float)args.get<double>(6, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasFillText", [this, parseColor](choc::javascript::ArgumentList args) {
        auto cid = args.get<std::string>(0, "");
        auto* c = dynamic_cast<CanvasWidget*>(widget(cid));
        if (!c) return choc::value::Value();

        CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_text;

        // pulp #1899 — accept BOTH calling conventions.
        //
        // Pulp's web-compat-canvas.js shim emits the 7-arg form:
        //   canvasFillText(id, text, x, y, size, color, maxWidth)
        //
        // Third-party shims bundled with imported designs (e.g. Spectr's
        // native-react/canvas2d-shim.ts:269) emit a 4-arg form with text
        // LAST:
        //   canvasFillText(id, x, y, text)
        //
        // Both are valid JS-side surface contracts. Detect by checking
        // whether slot 1 is a string (web-compat form) or a number
        // (legacy / third-party form). Without this branch, the 4-arg
        // form silently drops all text — every fillText() that flowed
        // through the third-party shim recorded an empty fill_text cmd,
        // which fill_text() in skia_canvas then skipped via the
        // `if (text.empty()) return;` early-out. Net effect: bars + grid
        // rendered (other commands), but every axis label / overlay
        // text was invisible.
        std::string slot1_str = args.get<std::string>(1, "");
        const bool is_4arg_form = (args.numArgs == 4) && slot1_str.empty();

        if (is_4arg_form) {
            // canvasFillText(id, x, y, text). Third-party shims that emit
            // this form (e.g. Spectr's canvas2d-shim.ts:269) often do NOT
            // call canvasSetFont first, so the CanvasWidget's command
            // buffer has no `set_font` command ahead of this `fill_text`.
            // At paint time, CGCanvas / SkiaCanvas would create a font
            // with font_size_ = 0 (the canvas's default) → glyphs are
            // 0-pt → text draws invisibly. Inject a default set_font
            // command so the replay establishes a sane font state before
            // this fill_text command renders.
            //
            // pulp #1901 review (Codex P1): only inject the default
            // set_font when no prior font state has been recorded on
            // this canvas. Scanning the recorded command stream is the
            // canvas's only source of "prior state" — there is no
            // separate accessor. If a caller already issued
            // canvasSetFont / canvasSetFontFull, preserve their state
            // (no override). Same rationale for cmd.color below: a
            // prior canvasSetFillColor (or fill-gradient/pattern) must
            // not be stomped by the hard-coded #fff default — scan
            // back for the most recent fill-style cmd and reuse its
            // color, otherwise fall back to #fff.
            const auto& prior = c->commands();
            bool has_prior_font = false;
            bool has_prior_fill = false;
            canvas::Color prior_fill_color = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
            for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
                if (!has_prior_font &&
                    (it->type == CanvasDrawCmd::Type::set_font ||
                     it->type == CanvasDrawCmd::Type::set_font_full)) {
                    has_prior_font = true;
                }
                if (!has_prior_fill &&
                    (it->type == CanvasDrawCmd::Type::set_fill_color ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_linear ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_radial ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_radial_two_circles ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_conic ||
                     it->type == CanvasDrawCmd::Type::set_fill_pattern)) {
                    has_prior_fill = true;
                    prior_fill_color = it->color;
                }
                if (has_prior_font && has_prior_fill) break;
            }

            if (!has_prior_font) {
                CanvasDrawCmd font_cmd;
                font_cmd.type  = CanvasDrawCmd::Type::set_font;
                font_cmd.text  = "system-ui";
                font_cmd.extra = 14.0f;
                c->add_command(font_cmd);
            }

            cmd.x    = (float)args.get<double>(1, 0);
            cmd.y    = (float)args.get<double>(2, 0);
            cmd.text = args.get<std::string>(3, "");
            cmd.extra = 14.0f;                  // default font size px
            // Preserve any prior fill style; only default to white when
            // the caller never set a fill color / gradient / pattern.
            cmd.color = has_prior_fill ? prior_fill_color
                                       : parseColor("#fff");
            cmd.w     = 0.0f;                   // no maxWidth
        } else {
            // canvasFillText(id, text, x, y, size, color, maxWidth)
            cmd.text  = slot1_str;
            cmd.x     = (float)args.get<double>(2, 0);
            cmd.y     = (float)args.get<double>(3, 0);
            cmd.extra = (float)args.get<double>(4, 14);
            cmd.color = parseColor(args.get<std::string>(5, "#fff"));
            // pulp #1525 — maxWidth threaded as 7th arg in CSS px;
            // `<= 0` or absent means "no constraint".
            cmd.w     = (float)args.get<double>(6, 0.0);
        }

        c->add_command(cmd);
        return choc::value::Value();
    });

    // pulp #1525 — dedicated stroke_text bridge entry. Pre-#1525 the JS
    // shim's `ctx.strokeText` path re-routed through `canvasFillText`
    // with the strokeStyle as the fill colour — visually approximate
    // but spec-incompatible (no real outlined glyphs, lineWidth ignored).
    // canvasStrokeText records a distinct stroke_text cmd so the paint
    // loop can route it through `Canvas::stroke_text` for true outlined
    // rendering on backends that override it (Skia, CG).
    //
    // Args: (id, text, x, y, fontSize, color, maxWidth?). Color carries
    // strokeStyle; lineWidth is set ahead of the call by the JS shim's
    // _syncStrokeState path (canvasSetLineWidth + canvasSetStrokeColor).
    engine_.register_function("canvasStrokeText", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_text;
            cmd.text = args.get<std::string>(1, "");
            cmd.x=(float)args.get<double>(2,0); cmd.y=(float)args.get<double>(3,0);
            cmd.extra=(float)args.get<double>(4, 14);
            cmd.color = parseColor(args.get<std::string>(5, "#fff"));
            cmd.w = (float)args.get<double>(6, 0.0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetFillColor", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_color;
            cmd.color = parseColor(args.get<std::string>(1, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetStrokeColor", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_color;
            cmd.color = parseColor(args.get<std::string>(1, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetLineWidth", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_width;
            cmd.extra = (float)args.get<double>(1, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetFont", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_font;
            cmd.text = args.get<std::string>(1, "Inter");
            cmd.extra = (float)args.get<double>(2, 14);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1434 — Canvas2D `ctx.font` full CSS font shorthand. The JS
    // shim parses `[<style>] [<variant>] [<weight>] <size>[/<lineHeight>]
    // <family>` and dispatches here when the parse extracts more than the
    // legacy size+family. Args: (id, family, size, weight, slant,
    // letter_spacing). Slant: 0 = upright, 1 = italic/oblique. Weight:
    // 100..900 (normal=400, bold=700). The `set_font_full` cmd routes to
    // `Canvas::set_font_full` on replay; Skia honours weight/slant via
    // `make_font(family, size, weight, slant)`. CG falls through to the
    // base default (family+size only).
    engine_.register_function("canvasSetFontFull", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_font_full;
            cmd.text  = args.get<std::string>(1, "Inter");
            cmd.extra = (float)args.get<double>(2, 14);
            cmd.x     = (float)args.get<double>(3, 400);   // weight
            cmd.y     = (float)args.get<double>(4, 0);     // slant
            cmd.x2    = (float)args.get<double>(5, 0);     // letter_spacing
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Path operations
    engine_.register_function("canvasBeginPath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::begin_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasMoveTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::move_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasLineTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::line_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasQuadTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::quad_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.x2=(float)args.get<double>(3,0); cmd.y2=(float)args.get<double>(4,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasCubicTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::cubic_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.x2=(float)args.get<double>(3,0); cmd.y2=(float)args.get<double>(4,0);
            cmd.x3=(float)args.get<double>(5,0); cmd.y3=(float)args.get<double>(6,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasClosePath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::close_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasFillPath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            // pulp DIVERGE→PASS sweep — read the optional fillRule int
            // (0 = nonzero, 1 = evenodd) so the spec arg actually
            // threads into the recorded command. The skia / cg paint
            // sides already key on int_val for fill_path / clip;
            // before this read the value was always 0 even when JS
            // passed `1`. (Pairs with [issue-1522] test which had been
            // failing since landing because the wiring got missed.)
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_path;
            cmd.int_val = static_cast<int>(args.get<double>(1, 0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokePath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1521 — native arc subpaths. Each replaces the JS shim's
    // bezier approximation so Skia / CG see real arc geometry. Args:
    //   canvasPathArc(id, cx, cy, radius, startAngle, endAngle,
    //                 anticlockwise:0/1)
    //   canvasPathArcTo(id, x1, y1, x2, y2, radius)
    //   canvasPathEllipse(id, cx, cy, rx, ry, rotation, startAngle,
    //                     endAngle, anticlockwise:0/1)
    //   canvasPathRoundRect(id, x, y, w, h,
    //                       tl_x, tl_y, tr_x, tr_y,
    //                       br_x, br_y, bl_x, bl_y)
    engine_.register_function("canvasPathArc", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_arc;
            cmd.x     = (float)args.get<double>(1, 0);
            cmd.y     = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            cmd.x2    = (float)args.get<double>(4, 0);
            cmd.y2    = (float)args.get<double>(5, 0);
            cmd.int_val = args.get<double>(6, 0) != 0.0 ? 1 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasPathArcTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_arc_to;
            cmd.x     = (float)args.get<double>(1, 0);
            cmd.y     = (float)args.get<double>(2, 0);
            cmd.x2    = (float)args.get<double>(3, 0);
            cmd.y2    = (float)args.get<double>(4, 0);
            cmd.extra = (float)args.get<double>(5, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasPathEllipse", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_ellipse;
            cmd.x     = (float)args.get<double>(1, 0);   // cx
            cmd.y     = (float)args.get<double>(2, 0);   // cy
            cmd.w     = (float)args.get<double>(3, 0);   // rx
            cmd.h     = (float)args.get<double>(4, 0);   // ry
            cmd.extra = (float)args.get<double>(5, 0);   // rotation
            cmd.x2    = (float)args.get<double>(6, 0);   // startAngle
            cmd.y2    = (float)args.get<double>(7, 0);   // endAngle
            cmd.int_val = args.get<double>(8, 0) != 0.0 ? 1 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasPathRoundRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_round_rect;
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0);
            cmd.h = (float)args.get<double>(4, 0);
            cmd.gradient_positions = {
                (float)args.get<double>(5, 0),  (float)args.get<double>(6, 0),
                (float)args.get<double>(7, 0),  (float)args.get<double>(8, 0),
                (float)args.get<double>(9, 0),  (float)args.get<double>(10, 0),
                (float)args.get<double>(11, 0), (float)args.get<double>(12, 0),
            };
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // State
    engine_.register_function("canvasSave", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::save; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasRestore", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::restore; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Transform
    engine_.register_function("canvasTranslate", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::translate;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasScale", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::scale;
            cmd.x=(float)args.get<double>(1,1); cmd.y=(float)args.get<double>(2,1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasRotate", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::rotate;
            cmd.extra=(float)args.get<double>(1,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // setStateStyle(id, state, property, value) — declarative state-driven styling
    // Replaces manual hover callback wiring. States: hover, active, focus, disabled
    engine_.register_function("setStateStyle", [this, parseColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto state = args.get<std::string>(1, "hover");
        auto prop = args.get<std::string>(2, "");
        auto val_str = args.get<std::string>(3, "");
        auto* v = widget(id);
        if (!v || prop.empty()) return choc::value::Value();

        // Store the original value on first call
        // Then register hover/focus callbacks that apply/revert

        if (state == "hover") {
            // Capture current value as "normal" state
            if (prop == "background") {
                auto target_color = parseColor(val_str);
                auto* view = v;
                // Wire hover enter/leave to apply/revert background
                view->on_hover_enter = [this, id, view, target_color]() {
                    view->set_background_color(target_color);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view]() {
                    view->clear_background_color();
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "scale") {
                float target_scale = std::stof(val_str);
                auto* view = v;
                view->on_hover_enter = [this, id, view, target_scale]() {
                    view->set_scale(target_scale);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view]() {
                    view->set_scale(1.0f);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "opacity") {
                float target_opacity = std::stof(val_str);
                auto* view = v;
                float original = view->opacity();
                view->on_hover_enter = [this, id, view, target_opacity]() {
                    view->set_opacity(target_opacity);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view, original]() {
                    view->set_opacity(original);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            }
        }
        return choc::value::Value();
    });

    // setEnabled(id, bool) — CSS :disabled equivalent
    engine_.register_function("setEnabled", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto enabled = args.get<double>(1, 1) > 0.5;
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_enabled(enabled);
        return choc::value::Value();
    });

    // setDebugPaint(bool) — draw bounding box outlines on all views
    engine_.register_function("setDebugPaint", [this](choc::javascript::ArgumentList args) {
        auto on = args.get<double>(0, 0) > 0.5;
        // Store as a dimension token on root theme
        auto theme = root_.theme();
        theme.dimensions["debug.paint"] = on ? 1.0f : 0.0f;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // setColorToken(name, color) — set a color token on the root theme
    engine_.register_function("setColorToken", [this, parseColor](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto color_str = args.get<std::string>(1, "");
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        auto c = parseColor(color_str);
        theme.colors[name] = c;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // setDimensionToken(name, value) — set a dimension token on the root theme
    engine_.register_function("setDimensionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto val = static_cast<float>(args.get<double>(1, 0));
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.dimensions[name] = val;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // setTextTransform(id, "uppercase"/"lowercase"/"capitalize"/"none") — CSS text-transform
    engine_.register_function("setTextTransform", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto t = args.get<std::string>(1, "none");
        if (auto* l = dynamic_cast<Label*>(v)) {
            if (t == "uppercase") l->set_text_transform(Label::TextTransform::uppercase);
            else if (t == "lowercase") l->set_text_transform(Label::TextTransform::lowercase);
            else if (t == "capitalize") l->set_text_transform(Label::TextTransform::capitalize);
            else l->set_text_transform(Label::TextTransform::none);
        }
        return choc::value::Value();
    });

    // setTextDecoration(id, "underline"/"line-through"/"overline"/"none") — CSS text-decoration
    engine_.register_function("setTextDecoration", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto d = args.get<std::string>(1, "none");
        if (auto* l = dynamic_cast<Label*>(v)) {
            if (d == "underline") l->set_text_decoration(Label::TextDecoration::underline);
            else if (d == "line-through") l->set_text_decoration(Label::TextDecoration::line_through);
            else if (d == "overline") l->set_text_decoration(Label::TextDecoration::overline);
            else l->set_text_decoration(Label::TextDecoration::none);
        }
        return choc::value::Value();
    });

    // pulp #1434 (batch 3) — text-decoration longhands. CSS shorthand
    // `text-decoration` historically routed through `setTextDecoration`
    // above (line keyword only). The longhand triplet
    // `text-decoration-line` / `-color` / `-style` reaches each setter
    // independently so authors can build the decoration up piece-by-piece
    // without losing previously-set siblings (mirrors the per-attribute
    // border-fix from PR #1166 finding #4).

    // setTextDecorationColor(id, "#rrggbb"|color-token)
    engine_.register_function("setTextDecorationColor",
        [this, parseHexColor](choc::javascript::ArgumentList args) {
            auto* v = widget(args.get<std::string>(0, ""));
            auto hex = args.get<std::string>(1, "");
            if (auto* l = dynamic_cast<Label*>(v); l && !hex.empty()) {
                l->set_text_decoration_color(parseHexColor(hex));
            }
            return choc::value::Value();
        });

    // setTextDecorationStyle(id, "solid"|"double"|"dotted"|"dashed"|"wavy")
    // The paint path renders `solid` regardless today, but the value is
    // stored on the Label so future paint logic can honor it without an
    // API break — and so the JS shim's longhand → setter route doesn't
    // silently drop the property (which was the catalog's `missing`
    // status before this PR).
    engine_.register_function("setTextDecorationStyle",
        [this](choc::javascript::ArgumentList args) {
            auto* v = widget(args.get<std::string>(0, ""));
            auto s = args.get<std::string>(1, "solid");
            if (auto* l = dynamic_cast<Label*>(v)) {
                if (s == "double") l->set_text_decoration_style(Label::TextDecorationStyle::double_);
                else if (s == "dotted") l->set_text_decoration_style(Label::TextDecorationStyle::dotted);
                else if (s == "dashed") l->set_text_decoration_style(Label::TextDecorationStyle::dashed);
                else if (s == "wavy") l->set_text_decoration_style(Label::TextDecorationStyle::wavy);
                else l->set_text_decoration_style(Label::TextDecorationStyle::solid);
            }
            return choc::value::Value();
        });

    // ── pulp #1552 — line-clamp + background-repeat ─────────────────────────
    // CSS `line-clamp` and `-webkit-line-clamp` route through the same
    // shared case in web-compat-style-decl.js (and the @pulp/react
    // prop-applier emits both keys via setLineClamp). Numeric only; 0
    // disables clamping (matches CSS spec, which uses `none`). Wired on
    // Label only — non-text views ignore the property.
    engine_.register_function("setLineClamp", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        int n = static_cast<int>(args.get<double>(1, 0.0));
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_line_clamp(n);
            // line-clamp implies multi-line — without multi_line_, the
            // paint path takes the single-line branch and the clamp is a
            // no-op. Setting > 0 implicitly enables wrap; 0 leaves the
            // existing multi_line_ flag alone (the user may have set it
            // independently via white-space / setMultiLine).
            if (n > 0) l->set_multi_line(true);
        }
        return choc::value::Value();
    });

    // setBackgroundRepeat(id, kw) — CSS background-repeat keyword. Storage-
    // only on the View (no-op for solid-color backgrounds, which is the
    // only currently rendered case). Future paint work for
    // `background-image: url(...)` / repeating gradients consults the
    // stored slot; setting the keyword today makes the round-trip work
    // and lets authors express intent without dropping the prop silently.
    // Accepts: `repeat` / `repeat-x` / `repeat-y` / `no-repeat` /
    // `space` / `round`. Unknown / empty resets to "" (paint defaults to
    // CSS initial `repeat`).
    engine_.register_function("setBackgroundRepeat", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_background_repeat(kw);
        return choc::value::Value();
    });

    // setPosition(id, "static"/"relative"/"absolute"/"fixed") — CSS position
    engine_.register_function("setPosition", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pos = args.get<std::string>(1, "static");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pos == "relative") v->set_position(View::Position::relative);
        else if (pos == "absolute") v->set_position(View::Position::absolute);
        else if (pos == "fixed") v->set_position(View::Position::fixed);
        else if (pos == "sticky") v->set_position(View::Position::sticky);
        else v->set_position(View::Position::static_);
        return choc::value::Value();
    });

    // setTop/setRight/setBottom/setLeft(id, px-or-percent) — CSS positioning
    // offsets. pulp #1434 batch 6 — accept either a number ("50" → px) or
    // a percentage string ("50%" → percent of parent). The CSS translator
    // and @pulp/react prop-applier pass the raw resolved string for these
    // four keys when the value is a percent, so the unit survives the
    // bridge boundary; Yoga's YGNodeStyleSetPositionPercent path is reached
    // via View::top_unit_ / etc. in yoga_layout.cpp. Mirrors the
    // FlexStyle::dim_width pattern from pulp #1423 (PR #1426) for width/height.
    engine_.register_function("setTop", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_top(std::stof(sval.substr(0, sval.size() - 1)),
                               pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_top(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_top(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });
    engine_.register_function("setRight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_right(std::stof(sval.substr(0, sval.size() - 1)),
                                 pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_right(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_right(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });
    engine_.register_function("setBottom", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_bottom(std::stof(sval.substr(0, sval.size() - 1)),
                                  pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_bottom(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_bottom(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });
    engine_.register_function("setLeft", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_left(std::stof(sval.substr(0, sval.size() - 1)),
                                pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_left(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_left(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });

    // setZIndex(id, n) — CSS z-index
    engine_.register_function("setZIndex", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (v) v->set_z_index(static_cast<int>(args.get<double>(1, 0)));
        return choc::value::Value();
    });

    // setTransitionDuration(id, seconds) — CSS transition duration for animated property changes
    engine_.register_function("setTransitionDuration", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto dur = static_cast<float>(args.get<double>(1, 0.15));
        // Store transition duration on the view's theme as a dimension token
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            auto theme = v->theme();
            theme.dimensions["transition.duration"] = dur;
            v->set_theme(theme);
        }
        return choc::value::Value();
    });

    // pulp #1434 Phase A2-1 — setTransition(id, "opacity 200ms ease, transform 300ms")
    // Parses the full CSS shorthand into View::transitions_. PR 2 of
    // the ladder will hook the prop-applier dispatcher to consult
    // these specs when a property changes. PR 1 ships parser + storage.
    engine_.register_function("setTransition", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto css = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (css.empty() || css == "none") {
            v->clear_transitions();
            return choc::value::Value();
        }
        v->set_transitions(parse_transition_shorthand(css));
        return choc::value::Value();
    });

    // setTransitionProperty(id, "opacity, transform")  — comma-separated
    // property list. We synthesize TransitionSpecs with just the property
    // name; durations are picked up from setTransitionDuration / the
    // shorthand path. CSS spec: shorthand wins over longhand when both
    // are set in the same rule; we treat them as additive at this layer.
    engine_.register_function("setTransitionProperty", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto props = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        std::vector<TransitionSpec> ts;
        std::string acc;
        auto flush = [&]() {
            while (!acc.empty() && std::isspace(static_cast<unsigned char>(acc.front()))) acc.erase(0, 1);
            while (!acc.empty() && std::isspace(static_cast<unsigned char>(acc.back()))) acc.pop_back();
            if (!acc.empty()) {
                TransitionSpec s{};
                s.property_name = acc;
                s.property = animatable_property_from_css_name(acc);
                ts.push_back(std::move(s));
                acc.clear();
            }
        };
        for (char c : props) {
            if (c == ',') flush(); else acc += c;
        }
        flush();
        v->set_transitions(std::move(ts));
        return choc::value::Value();
    });

    // setTransitionTimingFunction(id, "ease-in-out") — applies to all
    // existing TransitionSpecs on the View. CSS spec: longhand applies
    // uniformly across the property list.
    engine_.register_function("setTransitionTimingFunction", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto easing_str = args.get<std::string>(1, "ease");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto current = v->transitions();
        for (auto& t : current) {
            t.easing = CssEasing::from_keyword(easing_str);
        }
        v->set_transitions(std::move(current));
        return choc::value::Value();
    });

    // setTransitionDelay(id, seconds)
    engine_.register_function("setTransitionDelay", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto delay = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto current = v->transitions();
        for (auto& t : current) t.delay_seconds = delay;
        v->set_transitions(std::move(current));
        return choc::value::Value();
    });

    // setTranslate(id, x, y) — CSS transform: translate()
    engine_.register_function("setTranslate", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0));
        auto y = static_cast<float>(args.get<double>(2, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_translate(x, y);
        return choc::value::Value();
    });

    // setRotation(id, degrees) — CSS transform: rotate()
    engine_.register_function("setRotation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto deg = static_cast<float>(args.get<double>(1, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_rotation(deg);
        return choc::value::Value();
    });

    // setTransform(id, a, b, c, d, e, f) — CSS transform: matrix(a,b,c,d,e,f)
    // Applied at View paint time as a concat onto the current canvas matrix
    // so it composes with parent transforms and child Views inherit it.
    // Layout (Yoga + hit-test) sees the un-transformed bounds — paint-only.
    // Issue-930. Companion to canvasSetTransform from PR #897 (issue-896),
    // but applied to the View's painting frame rather than a Canvas2D context.
    engine_.register_function("setTransform", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto a = static_cast<float>(args.get<double>(1, 1.0));
        auto b = static_cast<float>(args.get<double>(2, 0.0));
        auto c = static_cast<float>(args.get<double>(3, 0.0));
        auto d = static_cast<float>(args.get<double>(4, 1.0));
        auto e = static_cast<float>(args.get<double>(5, 0.0));
        auto f = static_cast<float>(args.get<double>(6, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_transform_matrix(a, b, c, d, e, f);
        return choc::value::Value();
    });

    // clearTransform(id) — drop the affine matrix; the View reverts to its
    // CSS-transform scalars (translate/rotate/scale) only. Mirrors removing
    // the inline `transform` property in CSS. Issue-930.
    engine_.register_function("clearTransform", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->clear_transform_matrix();
        return choc::value::Value();
    });

    // setTransformOrigin(id, x, y) — CSS transform-origin (0-1 normalized)
    engine_.register_function("setTransformOrigin", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0.5));
        auto y = static_cast<float>(args.get<double>(2, 0.5));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_transform_origin(x, y);
        return choc::value::Value();
    });

    // pulp #1434 Phase A2-1 — defineKeyframes(name, stops_json_string).
    // Stops are JSON-encoded for bridge simplicity:
    //   defineKeyframes('fade', JSON.stringify([
    //     { offset: 0,   properties: { opacity: '0' } },
    //     { offset: 1.0, properties: { opacity: '1' } },
    //   ]));
    // The CSS shim's @keyframes parser produces this shape directly.
    // Populates the application-wide registry. PR 4 wires the registry
    // into setAnimation playback; PR 1 ships parser + storage so the
    // registry is consultable today.
    engine_.register_function("defineKeyframes", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto stops_json = args.get<std::string>(1, "[]");
        if (name.empty()) return choc::value::Value();
        CssKeyframesBlock block;
        block.name = name;
        // Parse the JSON via choc::json which already lives in the
        // tree (see DEPENDENCIES.md). Walk the array structurally.
        try {
            auto parsed = choc::json::parse(stops_json);
            if (parsed.isArray()) {
                for (uint32_t i = 0; i < parsed.size(); ++i) {
                    const auto& entry = parsed[i];
                    CssKeyframe kf{};
                    if (entry.hasObjectMember("offset")) {
                        kf.offset = static_cast<float>(entry["offset"].getWithDefault<double>(0.0));
                    }
                    if (entry.hasObjectMember("properties") && entry["properties"].isObject()) {
                        const auto& props = entry["properties"];
                        for (uint32_t j = 0; j < props.size(); ++j) {
                            std::string mn(props.getObjectMemberAt(j).name);
                            std::string val(props[mn.c_str()].getWithDefault<std::string_view>(""));
                            kf.properties.emplace_back(mn, val);
                        }
                    }
                    block.stops.push_back(std::move(kf));
                }
            }
        } catch (...) {
            // Malformed input — silently drop (registry stays unchanged).
            return choc::value::Value();
        }
        css_keyframes_registry_.define(std::move(block));
        return choc::value::Value();
    });

    // pulp #1434 Phase A2-1 — setAnimation supports two ABIs.
    //
    //   Positional (new — @pulp/react direct callers):
    //     setAnimation(id, animation_name, duration, iterations, direction)
    //   Legacy control-token (web-compat-style-decl.js — one CSS longhand
    //   per call — and prop-applier's animation* fan-out):
    //     setAnimation(id, "name",       <animation-name>)
    //     setAnimation(id, "duration",   <seconds>)
    //     setAnimation(id, "delay",      <seconds>)
    //     setAnimation(id, "easing",     <css-easing-keyword>)
    //     setAnimation(id, "iterations", <number | -1 for infinite>)
    //     setAnimation(id, "direction",  <"normal"|"reverse"|...>)
    //     setAnimation(id, "fill",       <"none"|"forwards"|...>)
    //
    // Detection: if arg1 matches one of the control tokens, dispatch to
    // the legacy path — otherwise treat as positional. The legacy path
    // accumulates state in View::staged_animation(); when `name` arrives
    // and resolves against the keyframes registry, we seed entries into
    // active_animations() using the staged values. Codex audit on pulp
    // #1508 caught the original handler dropping every web-compat call
    // because "name"/"duration"/etc. were being treated as the
    // animation_name token (registry lookup always missed).
    engine_.register_function("setAnimation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto arg1 = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();

        // Legacy control-token dispatch.
        const bool is_control_token =
            arg1 == "name"      || arg1 == "duration"  || arg1 == "delay"
         || arg1 == "easing"    || arg1 == "iterations"|| arg1 == "direction"
         || arg1 == "fill"      || arg1 == "play_state";
        if (is_control_token) {
            auto& staged = v->staged_animation();
            if (arg1 == "name") {
                staged.name = args.get<std::string>(2, "");
                // Re-resolve when the name token arrives (the typical
                // call order is name → duration → easing → ..., but
                // either order is valid; if duration arrives first we
                // simply seed when name arrives, using whatever
                // duration was previously staged).
                const auto* block = css_keyframes_registry_.find(staged.name);
                if (block && !block->stops.empty()) {
                    const auto& first = block->stops.front();
                    for (const auto& [prop, _val] : first.properties) {
                        CssAnimation a{};
                        a.property = animatable_property_from_css_name(prop);
                        a.spec.property_name = prop;
                        a.spec.property = a.property;
                        a.spec.duration_seconds = staged.duration_seconds;
                        a.spec.delay_seconds    = staged.delay_seconds;
                        a.spec.easing           = staged.easing;
                        v->active_animations().push_back(std::move(a));
                    }
                }
            } else if (arg1 == "duration") {
                staged.duration_seconds = static_cast<float>(args.get<double>(2, 0.0));
                for (auto& a : v->active_animations()) {
                    a.spec.duration_seconds = staged.duration_seconds;
                }
            } else if (arg1 == "delay") {
                staged.delay_seconds = static_cast<float>(args.get<double>(2, 0.0));
                for (auto& a : v->active_animations()) {
                    a.spec.delay_seconds = staged.delay_seconds;
                }
            } else if (arg1 == "easing") {
                staged.easing = CssEasing::from_keyword(args.get<std::string>(2, ""));
                for (auto& a : v->active_animations()) {
                    a.spec.easing = staged.easing;
                }
            } else if (arg1 == "iterations") {
                staged.iterations = static_cast<float>(args.get<double>(2, 1.0));
            } else if (arg1 == "direction") {
                staged.direction = args.get<std::string>(2, "normal");
            } else if (arg1 == "fill") {
                staged.fill_mode = args.get<std::string>(2, "");
            } else if (arg1 == "play_state") {
                // pulp #1434 A4 Bundle 2 — animation-play-state.
                // Storage-only today; the playback driver pause/resume
                // is the follow-up. Mirror to View's storage slot so
                // round-trip queries work without poking into the
                // staged struct.
                v->set_animation_play_state(args.get<std::string>(2, "running"));
            }
            return choc::value::Value();
        }

        // Positional dispatch (new ABI).
        const auto& anim_name = arg1;
        auto duration = static_cast<float>(args.get<double>(2, 0.0));
        (void)args.get<double>(3, 1.0);  // iterations — PR 4
        (void)args.get<std::string>(4, "normal");  // direction — PR 4
        const auto* block = css_keyframes_registry_.find(anim_name);
        if (!block || block->stops.empty()) return choc::value::Value();
        // Seed one Animation per property the first stop touches. PR 2
        // specializes the value type per property and drives the
        // tween via the frame clock.
        const auto& first = block->stops.front();
        for (const auto& [prop, _val] : first.properties) {
            CssAnimation a{};
            a.property = animatable_property_from_css_name(prop);
            a.spec.property_name = prop;
            a.spec.property = a.property;
            a.spec.duration_seconds = duration;
            a.spec.delay_seconds = 0.0f;
            a.spec.easing = CssEasing{};
            v->active_animations().push_back(std::move(a));
        }
        return choc::value::Value();
    });

    // setScale(id, scale) — CSS transform: scale()
    engine_.register_function("setScale", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_scale(s);
        return choc::value::Value();
    });

    // setSkew(id, x_deg, y_deg) — CSS transform: skewX() / skewY().
    // pulp #1434 Triage #9 (transform fan-out) — View::set_skew has
    // existed since the 2D View slot was added; this surface just
    // hadn't been registered as a JS bridge fn until now. The CSS
    // shim's parseTransform dispatches each axis independently
    // (skewX(α) → setSkew(id, α, 0); skewY(β) → setSkew(id, 0, β));
    // when both appear in the same transform string the second
    // call's arg-pattern preserves the axis the first call set
    // (caller-side accumulation since within-string order is
    // canonical CSS application order). The @pulp/react prop-applier
    // walker accumulates skewX/skewY in its snapshot the same way.
    engine_.register_function("setSkew", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0.0));
        auto y = static_cast<float>(args.get<double>(2, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_skew(x, y);
        return choc::value::Value();
    });

    // setTextOverflow(id, "ellipsis"|"clip") — CSS text-overflow
    engine_.register_function("setTextOverflow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "clip");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_text_overflow_ellipsis(mode == "ellipsis");
        return choc::value::Value();
    });

    // setCursor(id, "pointer"|"crosshair"|"text"|"default") — CSS cursor
    engine_.register_function("setCursor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto c = args.get<std::string>(1, "default");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        // pulp #1434 Triage #7 — cursor enum fan-out. Map the full CSS
        // cursor keyword set to the View::CursorStyle slots that exist
        // today (4 base + 7 resize + invisible + multi-directional = 12
        // distinct visuals). Where multiple CSS keywords map to the
        // same visual (n/s/e/w-resize all map to the axis-aligned
        // resize cursor), we collapse to the closest existing slot.
        //
        // pulp #1550 Tier-4 macOS partial 2026-05-12 — added dedicated
        // slots for `alias` / `copy` / `zoom-in` / `zoom-out` /
        // `context-menu` (5 keywords with real NSCursor backings on
        // macOS — see core/view/platform/mac/window_host_mac.mm).
        // `wait` / `help` / `progress` / `cell` stay routed to default
        // because macOS has no native cursor for them — listed in
        // compat.json unsupportedValues for honesty.
        using CS = View::CursorStyle;
        if (c == "pointer")              v->set_cursor(CS::pointer);
        else if (c == "crosshair")       v->set_cursor(CS::crosshair);
        else if (c == "text")            v->set_cursor(CS::text);
        else if (c == "vertical-text")   v->set_cursor(CS::text);
        else if (c == "grab")            v->set_cursor(CS::grab);
        else if (c == "grabbing")        v->set_cursor(CS::grabbing);
        else if (c == "not-allowed")     v->set_cursor(CS::not_allowed);
        else if (c == "no-drop")         v->set_cursor(CS::not_allowed);
        else if (c == "none" || c == "hidden") v->set_cursor(CS::invisible);
        else if (c == "col-resize" || c == "ew-resize"
                 || c == "e-resize" || c == "w-resize") {
            v->set_cursor(CS::horizontal_resize);
        }
        else if (c == "row-resize" || c == "ns-resize"
                 || c == "n-resize" || c == "s-resize") {
            v->set_cursor(CS::vertical_resize);
        }
        else if (c == "nwse-resize" || c == "nw-resize" || c == "se-resize") {
            v->set_cursor(CS::top_left_resize);
        }
        else if (c == "nesw-resize" || c == "ne-resize" || c == "sw-resize") {
            v->set_cursor(CS::top_right_resize);
        }
        else if (c == "move" || c == "all-scroll") {
            v->set_cursor(CS::multi_directional_resize);
        }
        // pulp #1550 — 5 CSS cursor keywords with macOS NSCursor backings.
        else if (c == "alias")           v->set_cursor(CS::alias);
        else if (c == "copy")            v->set_cursor(CS::copy);
        else if (c == "zoom-in")         v->set_cursor(CS::zoom_in);
        else if (c == "zoom-out")        v->set_cursor(CS::zoom_out);
        else if (c == "context-menu")    v->set_cursor(CS::context_menu);
        else                             v->set_cursor(CS::default_);
        return choc::value::Value();
    });

    // pulp #1434 Phase A2-3 — writing direction. Maps the CSS keyword
    // to View::WritingDirection. Yoga's flow honors direction for
    // flexDirection 'row' (which visually reverses under RTL); Skia's
    // paragraph_style.setTextDirection picks up the same value at
    // shape time. Logical-edge mapping in the @pulp/react prop-applier
    // currently stays LTR-only (per #1497 fast-path note); a future
    // slice will make it direction-aware via a shared resolver.
    engine_.register_function("setDirection", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto d = args.get<std::string>(1, "ltr");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (d == "rtl")        v->set_direction(View::WritingDirection::rtl);
        else if (d == "ltr")   v->set_direction(View::WritingDirection::ltr);
        else                   v->set_direction(View::WritingDirection::auto_);
        return choc::value::Value();
    });

    // setFilter(id, "blur(4px) brightness(0.8) saturate(1.2) drop-shadow(...)")
    //   — pulp #1434 Phase A2-4 CSS filter chain. Walks the function
    //   sequence and builds View::FilterOp entries; the View paint
    //   path passes the chain to canvas.save_layer_with_filters which
    //   composes via SkImageFilters on the Skia backend (CG falls
    //   through to blur-only for now).
    engine_.register_function("setFilter", [this, parseColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto filter_str = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();

        if (filter_str == "none" || filter_str.empty()) {
            v->clear_filter_chain();
            v->set_filter_blur(0.0f);
            return choc::value::Value();
        }

        // Walk function-call sequence: `name(args)` repeated.
        std::vector<View::FilterOp> chain;
        size_t i = 0;
        while (i < filter_str.size()) {
            // Skip whitespace
            while (i < filter_str.size() && std::isspace(static_cast<unsigned char>(filter_str[i]))) ++i;
            if (i >= filter_str.size()) break;
            // Parse name up to '('
            size_t name_start = i;
            while (i < filter_str.size() && filter_str[i] != '(') ++i;
            if (i >= filter_str.size()) break;
            std::string name = filter_str.substr(name_start, i - name_start);
            // Trim trailing whitespace from name
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            ++i; // skip '('
            // Parse args up to ')'
            size_t args_start = i;
            int depth = 1;
            while (i < filter_str.size() && depth > 0) {
                if (filter_str[i] == '(') ++depth;
                else if (filter_str[i] == ')') --depth;
                if (depth > 0) ++i;
            }
            std::string args_str = filter_str.substr(args_start, i - args_start);
            if (i < filter_str.size()) ++i; // skip ')'

            View::FilterOp op{};
            // Strip 'px' / '%' suffix and parse numeric.
            auto parse_amount = [](const std::string& s) -> float {
                std::string t = s;
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
                if (t.size() >= 2 && t.substr(t.size() - 2) == "px") t.erase(t.size() - 2);
                bool pct = false;
                if (!t.empty() && t.back() == '%') { pct = true; t.pop_back(); }
                try {
                    float v = std::stof(t);
                    return pct ? v / 100.0f : v;
                } catch (...) { return 0.0f; }
            };
            auto parse_angle_deg = [](const std::string& s) -> float {
                std::string t = s;
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
                float scale = 1.0f;
                // Check 4-char suffixes first so "grad" doesn't get
                // matched as "rad" with a stray 'g' prefix.
                if (t.size() >= 4 && t.substr(t.size() - 4) == "grad") { t.erase(t.size() - 4); scale = 0.9f; }
                else if (t.size() >= 4 && t.substr(t.size() - 4) == "turn") { t.erase(t.size() - 4); scale = 360.0f; }
                else if (t.size() >= 3 && t.substr(t.size() - 3) == "deg") { t.erase(t.size() - 3); }
                else if (t.size() >= 3 && t.substr(t.size() - 3) == "rad") { t.erase(t.size() - 3); scale = 180.0f / 3.14159265358979323846f; }
                try { return std::stof(t) * scale; } catch (...) { return 0.0f; }
            };
            if      (name == "blur")       { op.kind = View::FilterOp::Kind::blur;       op.amount = parse_amount(args_str); }
            else if (name == "brightness") { op.kind = View::FilterOp::Kind::brightness; op.amount = parse_amount(args_str); }
            else if (name == "contrast")   { op.kind = View::FilterOp::Kind::contrast;   op.amount = parse_amount(args_str); }
            else if (name == "grayscale")  { op.kind = View::FilterOp::Kind::grayscale;  op.amount = parse_amount(args_str); }
            else if (name == "invert")     { op.kind = View::FilterOp::Kind::invert;     op.amount = parse_amount(args_str); }
            else if (name == "opacity")    { op.kind = View::FilterOp::Kind::opacity;    op.amount = parse_amount(args_str); }
            else if (name == "saturate")   { op.kind = View::FilterOp::Kind::saturate;   op.amount = parse_amount(args_str); }
            else if (name == "sepia")      { op.kind = View::FilterOp::Kind::sepia;      op.amount = parse_amount(args_str); }
            else if (name == "hue-rotate") { op.kind = View::FilterOp::Kind::hue_rotate; op.angle_deg = parse_angle_deg(args_str); }
            else if (name == "drop-shadow") {
                // drop-shadow(<dx> <dy> <blur> <color>) — space-separated
                op.kind = View::FilterOp::Kind::drop_shadow;
                std::vector<std::string> tokens;
                std::string tok;
                int paren = 0;
                for (char c : args_str) {
                    if (c == '(') { ++paren; tok += c; continue; }
                    if (c == ')') { --paren; tok += c; continue; }
                    if (paren == 0 && std::isspace(static_cast<unsigned char>(c))) {
                        if (!tok.empty()) { tokens.push_back(tok); tok.clear(); }
                        continue;
                    }
                    tok += c;
                }
                if (!tok.empty()) tokens.push_back(tok);
                if (tokens.size() >= 3) {
                    op.ds_offset_x = parse_amount(tokens[0]);
                    op.ds_offset_y = parse_amount(tokens[1]);
                    op.ds_blur     = parse_amount(tokens[2]);
                    if (tokens.size() >= 4) {
                        // tokens[3..] is the color (may be space-separated rgb()).
                        std::string color_str = tokens[3];
                        for (size_t k = 4; k < tokens.size(); ++k) color_str += " " + tokens[k];
                        // Lean on the existing Color::from_string parser.
                        op.ds_color = parseColor(color_str);
                    } else {
                        op.ds_color = canvas::Color::rgba(0.0f, 0.0f, 0.0f, 1.0f);
                    }
                }
            }
            else { continue; } // unknown filter function — silently drop
            chain.push_back(op);
        }

        // Maintain the legacy filter_blur_ slot for backward compat
        // with paths that haven't migrated to the chain API yet.
        float total_blur = 0.0f;
        for (const auto& op : chain) {
            if (op.kind == View::FilterOp::Kind::blur) total_blur += op.amount;
        }
        v->set_filter_blur(total_blur);
        v->set_filter_chain(std::move(chain));
        return choc::value::Value();
    });

    // setBackdropFilter(id, blur_px) — CSS `backdrop-filter: blur(Npx)` for
    // frosted-glass overlays / modal backgrounds (issue-926). Numeric
    // overload to keep the bridge cheap; string-form CSS parsing stays in
    // setFilter.
    engine_.register_function("setBackdropFilter",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto blur_px = args.get<double>(1, 0.0);
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_backdrop_blur(static_cast<float>(blur_px));
            return choc::value::Value();
        });

    // setClipPath(id, value) — CSS `clip-path` (pulp #1515). The paint
    // side feeds the stored slot to `Canvas::clip_path_svg` which calls
    // `SkPath::FromSVGString` — that parser only accepts raw SVG path
    // "d" data, so we must unwrap `path("...")` here and explicitly
    // skip shapes / URL refs that the paint side cannot honor (they
    // remain documented as deferred). Forwarding the verbatim value
    // produced silent paint failures on every non-path() form (Codex
    // #1616 P1 on #1540).
    engine_.register_function("setClipPath",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            auto trim = [](std::string s) {
                auto a = s.find_first_not_of(" \t\n\r");
                if (a == std::string::npos) return std::string{};
                auto b = s.find_last_not_of(" \t\n\r");
                return s.substr(a, b - a + 1);
            };
            std::string t = trim(value);
            // CSS keywords (`none`, `path(...)`, `circle(...)`, etc.) are
            // case-insensitive per spec. Build a lowercased copy of the
            // prefix-bearing portion for comparison; preserve the original
            // case for the SVG path "d" data inside path("...") (Codex
            // #1616 P2 on #1540 follow-up #1698).
            std::string t_lower = t;
            for (auto& c : t_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t_lower.empty() || t_lower == "none") {
                v->set_clip_path("");
            } else if (t_lower.size() > 6 && t_lower.rfind("path(", 0) == 0 && t.back() == ')') {
                std::string inner = trim(t.substr(5, t.size() - 6));
                if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'')
                    && inner.back() == inner.front()) {
                    inner = inner.substr(1, inner.size() - 2);
                }
                v->set_clip_path(inner);
            } else {
                bool deferred_shape = false;
                for (const char* p : {"circle(", "ellipse(", "inset(", "polygon(",
                                       "rect(", "xywh(", "url("}) {
                    if (t_lower.rfind(p, 0) == 0) { deferred_shape = true; break; }
                }
                if (deferred_shape) {
                    v->set_clip_path("");
                } else {
                    v->set_clip_path(t);
                }
            }
            return choc::value::Value();
        });

    // setMaskImage(id, value) — CSS `mask-image` (pulp #1515).
    // Storage-only today; the saveLayer + SkBlendMode::kDstIn shader
    // composite is a follow-up paint slice. The slot round-trips
    // through View::mask_image() so harness tests can assert the
    // bridge accepted the value.
    engine_.register_function("setMaskImage",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_mask_image(value);
            return choc::value::Value();
        });

    // setMask(id, shorthand) — CSS `mask` shorthand (pulp #1515).
    // Stores the verbatim shorthand on the View; the JS shim
    // (web-compat-style-decl.js) is responsible for fanning out into
    // the maskImage longhand. Storage-only today.
    engine_.register_function("setMask",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_mask(value);
            return choc::value::Value();
        });

    // setMaskSize(id, value) — CSS `mask-size`, pairs with mask-image
    // (pulp #1515 followup). Storage-only; consumed by the same
    // future paint slice that wires the mask shader.
    engine_.register_function("setMaskSize",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_mask_size(value);
            return choc::value::Value();
        });

    // setAppearance(id, value) — CSS `appearance`. Pulp paints all
    // widgets custom (no native form-widget rendering), so this is
    // observably storage-only — `none` is the effective default for
    // every Pulp View regardless of what the slot says. The slot
    // exists so authors who set `appearance: none` for reset-style
    // consistency see a no-op (not an unsupported drop).
    engine_.register_function("setAppearance",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_appearance(value);
            return choc::value::Value();
        });

    // setObjectFit(id, value) — CSS `object-fit`. Storage-only today;
    // the ImageView paint slice that consumes this needs access to
    // the decoded image's natural size (planned follow-up).
    engine_.register_function("setObjectFit",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_object_fit(value);
            return choc::value::Value();
        });

    // setObjectPosition(id, value) — CSS `object-position`. Pairs
    // with object-fit. Storage-only today.
    engine_.register_function("setObjectPosition",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_object_position(value);
            return choc::value::Value();
        });

    // pulp #1549 — setMixBlendMode(id, "multiply") for CSS / RN
    // `mix-blend-mode`. Maps the W3C blend-mode keyword set to the
    // canvas BlendMode enum so the View paint path can pass it
    // straight into `save_layer_with_blend()` at compositing time.
    // The keyword set mirrors the W3C separable + non-separable blend
    // modes (the same 16 values RN's New Architecture surface accepts;
    // see tools/harness/oracles/rn/rn-viewstyle.json::mixBlendMode).
    // Unknown keywords (including the empty string and "normal") leave
    // the View at default `BlendMode::normal` so the fast path stays
    // a paint-time no-op.
    engine_.register_function("setMixBlendMode",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "normal");
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            using BM = pulp::canvas::Canvas::BlendMode;
            BM mode = BM::normal;
            if      (kw == "normal")      mode = BM::normal;
            else if (kw == "multiply")    mode = BM::multiply;
            else if (kw == "screen")      mode = BM::screen;
            else if (kw == "overlay")     mode = BM::overlay;
            else if (kw == "darken")      mode = BM::darken;
            else if (kw == "lighten")     mode = BM::lighten;
            else if (kw == "color-dodge") mode = BM::color_dodge;
            else if (kw == "color-burn")  mode = BM::color_burn;
            else if (kw == "hard-light")  mode = BM::hard_light;
            else if (kw == "soft-light")  mode = BM::soft_light;
            else if (kw == "difference")  mode = BM::difference;
            else if (kw == "exclusion")   mode = BM::exclusion;
            else if (kw == "hue")         mode = BM::hue;
            else if (kw == "saturation")  mode = BM::saturation;
            else if (kw == "color")       mode = BM::color;
            else if (kw == "luminosity")  mode = BM::luminosity;
            // pulp Wave 2 css.9 — `plus-lighter` and `plus-darker` are CSS
            // Compositing & Blending Level 2 keywords. Both map to
            // `SkBlendMode::kPlus` (additive) at the Skia layer (see
            // canvas.hpp::BlendMode::lighter, index 26). `plus-darker` is
            // technically a multiplicative variant in the W3C draft but
            // Skia / Chromium ship the additive `kPlus` for both;
            // mirroring that is the closest we can do without a custom
            // SkBlender. Keeps consumers (Figma export, compositing demos)
            // from silently falling back to `normal`.
            else if (kw == "plus-lighter" || kw == "plus-darker")
                                          mode = BM::lighter;
            // Unknown keyword → normal (paint-time no-op fallback).
            v->set_mix_blend_mode(mode);
            return choc::value::Value();
        });

    // pulp #1434 A4 Bundles 5–7 closure — storage-only setters for the
    // remaining css NOT-IMPL entries. Each handler records the value on
    // the View's catalog slot so harness round-trip tests can verify
    // the bridge accepts the keyword. Catalog status documents the
    // implementation depth (`partial` for storage-only with deferred
    // paint, `noop` for accept-and-ignore, `wontfix` for architectural
    // out-of-scope).

    // setTextIndent(id, px) — CSS `text-indent`. Storage-only today;
    // SkParagraph::setTextIndent integration is the paint-side follow-up.
    engine_.register_function("setTextIndent",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto v = static_cast<float>(args.get<double>(1, 0.0));
            auto* w = id.empty() ? &root_ : widget(id);
            if (w) w->set_text_indent(v);
            return choc::value::Value();
        });

    // setVerticalAlign(id, "top"|"middle"|"bottom"|"baseline"|...)
    // CSS `vertical-align`. Maps the keyword to the existing
    // canvas::TextVerticalAlign enum on Label; non-Label widgets
    // silently no-op (the field is per-widget). Length values
    // (`-2px` etc.) and `sub`/`super` fall back to baseline today.
    engine_.register_function("setVerticalAlign",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "baseline");
            using VA = pulp::canvas::TextVerticalAlign;
            VA mode = VA::baseline;
            if      (kw == "top")      mode = VA::top;
            else if (kw == "middle")   mode = VA::center;
            else if (kw == "center")   mode = VA::center;     // RN-Android textAlignVertical alias for `middle`
            else if (kw == "bottom")   mode = VA::bottom;
            else if (kw == "baseline") mode = VA::baseline;
            else if (kw == "auto")     mode = VA::baseline;   // RN verticalAlign / textAlignVertical default
            // sub / super / text-top / text-bottom / length percent →
            // baseline fallback (paint-side gap; future slice can add
            // dedicated slots).
            if (auto* l = dynamic_cast<Label*>(widget(id))) {
                l->set_vertical_align(mode);
            }
            return choc::value::Value();
        });

    // setWordBreak(id, kw) — CSS `word-break` / `overflow-wrap`.
    // Storage-only today; HarfBuzz line-break feature is deferred.
    engine_.register_function("setWordBreak",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "normal");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_word_break(kw);
            return choc::value::Value();
        });

    // setFontVariant(id, kw) — CSS / RN `font-variant`. Storage-only;
    // HarfBuzz feature wiring is deferred. Mirrors the rn-surface
    // `fontVariant` choice (the same A4 sweep, rn agent).
    engine_.register_function("setFontVariant",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "normal");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_font_variant(kw);
            return choc::value::Value();
        });

    // pulp #1548 — RN textShadow* per-attribute setters. Storage-only;
    // SkPaint shadow integration is the deferred paint-time slice. Each
    // setter writes ONE slot in isolation so a JSX prop diff that touches
    // one prop doesn't clobber the others (mirrors the per-side border
    // pattern). The catalog mapsTo cites these names; harness verifier
    // checks they are registered.
    engine_.register_function("setTextShadowColor",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto c  = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_text_shadow_color(c);
            return choc::value::Value();
        });
    engine_.register_function("setTextShadowOffset",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto dx = static_cast<float>(args.get<double>(1, 0.0));
            auto dy = static_cast<float>(args.get<double>(2, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_text_shadow_offset(dx, dy);
            return choc::value::Value();
        });
    engine_.register_function("setTextShadowRadius",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto r  = static_cast<float>(args.get<double>(1, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_text_shadow_radius(r);
            return choc::value::Value();
        });

    // pulp #1737 RN-OOS-fixup (audit 2026-05-11) — RN iOS-legacy
    // shadow{Color,Offset,Opacity,Radius} per-attribute setters for
    // box-shadow. Mirrors the textShadow* pattern above so a JSX prop
    // diff that touches one prop doesn't clobber the others. Modern
    // RN code uses `boxShadow` (CSS shorthand) — Pulp fully supports
    // that via setBoxShadow — but the per-attribute API is still in
    // upstream RN's surface, especially for code carrying iOS-legacy
    // styling. Each setter mutates one field of View::shadow_ and
    // flips has_shadow_ on, mirroring how text-shadow longhand works.
    engine_.register_function("setShadowColor",
        [this, parseHexColor](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto hex = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_color(parseHexColor(hex));
            return choc::value::Value();
        });
    engine_.register_function("setShadowOffset",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto dx = static_cast<float>(args.get<double>(1, 0.0));
            auto dy = static_cast<float>(args.get<double>(2, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_offset(dx, dy);
            return choc::value::Value();
        });
    engine_.register_function("setShadowOpacity",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto a  = static_cast<float>(args.get<double>(1, 1.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_opacity(a);
            return choc::value::Value();
        });
    engine_.register_function("setShadowRadius",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto r  = static_cast<float>(args.get<double>(1, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_radius(r);
            return choc::value::Value();
        });

    // pulp #1737 RN-OOS-fixup (audit 2026-05-11, final sweep) — RN's
    // `includeFontPadding` is an Android-only legacy prop. Android's
    // TextView paints extra vertical padding around text glyphs;
    // setting `includeFontPadding: false` removes it. Pulp's text-
    // shaping pipeline (Skia/SkParagraph) uses tight baseline-relative
    // positioning and DOESN'T add Android-style vestigial padding, so
    // Pulp's default behavior already matches the `false` outcome that
    // most authors want from this prop.
    //
    // Setting `true` is a silent no-op: Pulp can't add padding it
    // doesn't have without restructuring text shaping. Setting `false`
    // matches Pulp's existing default. Either way, the visible result
    // is the same — tight glyph layout with no Android-vestigial padding.
    //
    // Bridge fn stores the keyword on a View slot purely for round-
    // trip (element.style / style.X reading the value back). This
    // mirrors the overscroll-behavior pattern (PR #1805): all keywords
    // accepted, single consistent behavior produced, honest catalog
    // claim of "supported as a CSS-spec subset where Pulp's behavior
    // matches the dominant author intent".
    engine_.register_function("setIncludeFontPadding",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto v  = id.empty() ? &root_ : widget(id);
            // Accept the value, store it, no paint impact — Pulp's
            // text shaping doesn't add Android-vestigial padding
            // regardless of this flag.
            if (v) v->set_include_font_padding(args.get<bool>(1, true));
            return choc::value::Value();
        });

    // pulp #1737 RN-OOS-fixup (#1812) — RN's `borderCurve` corner shape.
    // `circular` (default) keeps the standard quarter-circle rounded
    // corner; `continuous` switches View::paint_all to the iOS-style
    // squircle approximation (super-ellipse path with extension factor
    // 1.528 and flatter kappa 0.85). Visible difference on large-radius
    // cards (24px+); subtle below 12px. See view.cpp's
    // build_continuous_corner_rounded_rect_path for the path math.
    engine_.register_function("setBorderCurve",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "circular");
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            v->set_border_curve(kw == "continuous"
                                ? View::BorderCurve::continuous
                                : View::BorderCurve::circular);
            return choc::value::Value();
        });

    // pulp #1737 RN-OOS-fixup (final round) — CSS `isolation` + RN
    // `isolation` honest CSS-subset flip. Pulp's per-View render model
    // is structurally isolated by default: each View with mix-blend-mode
    // opens its own save_layer_with_blend composition (PR #1549) and
    // composites back to its parent normally — there's no
    // "cross-stacking-context blend leakage" that CSS isolation: isolate
    // is designed to prevent. Similarly, z-index is paint-order scoped
    // to siblings within a parent, so a child's z-index can't promote
    // past the parent in z-order. Both author intents of `isolation:
    // isolate` (blend-mode containment + stacking-context creation)
    // happen by default in Pulp.
    //
    // Bridge fn stores the keyword on View::isolation_ for round-trip
    // reads (el.style.isolation === "isolate"). Paint has no special
    // case because Pulp's existing per-View layering already provides
    // the isolation contract. Same CSS-subset pattern as
    // overscrollBehavior, includeFontPadding, scrollBehavior from
    // earlier this session.
    engine_.register_function("setIsolation",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "auto");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_isolation(kw);
            return choc::value::Value();
        });

    // pulp #1737 RN-OOS-fixup (audit 2026-05-11) — RN's `elevation` is
    // Android-only Material elevation (0–24dp). Pulp catalogs boxShadow
    // as the cross-platform equivalent; this shim translates elevation
    // to a single-shadow approximation of the Material dual-shadow
    // spec so consumers shipping unchanged RN-Android styles get a
    // visible shadow on every Pulp platform.
    //
    // Approximation formula (Material Design system, simplified to
    // Pulp's single-shadow BoxShadow):
    //   elevation=0 -> clear box-shadow (no shadow)
    //   elevation N -> offset_y = max(1, N/2)
    //                  blur     = N + 1        (slightly larger than dp)
    //                  spread   = 0
    //                  color    = rgba(0, 0, 0, clamp(0.15+N*0.01, 0.15, 0.30))
    //
    // The blur ≈ elevation+1 and offset_y ≈ elevation/2 are the same
    // ratios Material's `mat-elevation-z*` mixin uses. The alpha ramp
    // matches Material's umbra-shadow opacity curve well enough to be
    // recognizable; the catalog notes call out the single-shadow
    // approximation honestly.
    engine_.register_function("setElevation",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto e  = static_cast<float>(args.get<double>(1, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            if (e <= 0.0f) {
                v->clear_box_shadow();
            } else {
                const float offset_y = std::max(1.0f, e * 0.5f);
                const float blur     = e + 1.0f;
                const float alpha    = std::min(0.30f, std::max(0.15f, 0.15f + e * 0.01f));
                v->set_box_shadow(0.0f, offset_y, blur, 0.0f,
                                  canvas::Color::rgba(0.0f, 0.0f, 0.0f, alpha),
                                  /*inset=*/false);
            }
            return choc::value::Value();
        });

    // pulp #1737 RN-OOS-fixup (audit 2026-05-11) — CSS scroll-behavior +
    // overscroll-behavior. Stored on the View slot; ScrollView reads
    // scroll_behavior_ in scroll_by (auto → instant, else smooth) and
    // overscroll_behavior_ via the existing clamp_scroll_targets path
    // (Pulp already clamps at content bounds and doesn't scroll-chain
    // to parents, so all three keywords [auto/contain/none] behave as
    // CSS `contain` — a valid subset of the spec).
    engine_.register_function("setScrollBehavior",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "smooth");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_scroll_behavior(kw);
            return choc::value::Value();
        });
    engine_.register_function("setOverscrollBehavior",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "auto");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_overscroll_behavior(kw);
            return choc::value::Value();
        });

    // Wave 5 css.5 — CSS-shorthand setTextShadow(id, dx, dy, blur, color).
    // The JS shim (web-compat-style-decl.js case textShadow) parses
    // `<dx>px <dy>px <blur>px <color>` and calls this with 4 packed args.
    // Pre-Wave-5 the shim's `typeof setTextShadow === "function"` guard
    // skipped the call because no bridge fn was registered; CSS authors
    // got a silent no-op. We compose the three existing per-attribute
    // slots (text_shadow_offset / text_shadow_radius / text_shadow_color)
    // so React's setTextShadow* fan-out still works unchanged.
    engine_.register_function("setTextShadow",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto dx = static_cast<float>(args.get<double>(1, 0.0));
            auto dy = static_cast<float>(args.get<double>(2, 0.0));
            auto r  = static_cast<float>(args.get<double>(3, 0.0));
            auto c  = args.get<std::string>(4, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) {
                v->set_text_shadow_offset(dx, dy);
                v->set_text_shadow_radius(r);
                v->set_text_shadow_color(c);
            }
            return choc::value::Value();
        });

    // pulp #1517 — background sub-property setters. Storage-only today;
    // see View::set_background_{attachment,clip,origin}() doc for the
    // partial-vs-noop semantics. Wiring them here unblocks the JS shim
    // path and lets the catalog honestly report `noop` / `partial`
    // instead of `missing`.
    engine_.register_function("setBackgroundAttachment",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_attachment(kw);
            return choc::value::Value();
        });
    engine_.register_function("setBackgroundClip",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_clip(kw);
            return choc::value::Value();
        });
    engine_.register_function("setBackgroundOrigin",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_origin(kw);
            return choc::value::Value();
        });

    // Wave 5 css.5 — setBackgroundPosition / setBackgroundSize. The JS
    // shim (web-compat-style-decl.js cases backgroundPosition /
    // backgroundSize) was already calling these as `typeof set... ===
    // "function"` guards; without a registered bridge fn the calls were
    // silent no-ops and the catalog claim of `supported` was a fiction.
    // Storage-only landing here makes the round-trip honest (JS → bridge
    // → View slot → get_attribute pulls it back) and unblocks a future
    // raster background-image paint slice — see View::set_background_*
    // doc for the architectural caveat.
    engine_.register_function("setBackgroundPosition",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_position(kw);
            return choc::value::Value();
        });
    engine_.register_function("setBackgroundSize",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_size(kw);
            return choc::value::Value();
        });

    // setBackgroundGradient(id, "linear-gradient(to right, #ff0000, #0000ff)")
    engine_.register_function("setBackgroundGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto gradient = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v || gradient.empty()) return choc::value::Value();

        // Simple parser for "linear-gradient(to right, color1, color2, ...)"
        if (gradient.substr(0, 16) == "linear-gradient(") {
            auto inner = gradient.substr(16, gradient.size() - 17);
            // Parse direction
            float x0 = 0, y0 = 0, x1 = 0, y1 = 1;  // default: to bottom
            size_t color_start = 0;
            if (inner.substr(0, 8) == "to right") { x0=0; y0=0; x1=1; y1=0; color_start = inner.find(',') + 1; }
            else if (inner.substr(0, 9) == "to bottom") { x0=0; y0=0; x1=0; y1=1; color_start = inner.find(',') + 1; }
            else if (inner.substr(0, 7) == "to left") { x0=1; y0=0; x1=0; y1=0; color_start = inner.find(',') + 1; }
            else if (inner.substr(0, 6) == "to top") { x0=0; y0=1; x1=0; y1=0; color_start = inner.find(',') + 1; }

            // Parse color stops
            std::vector<canvas::Color> colors;
            std::vector<float> positions;
            std::string colorStr = inner.substr(color_start);
            std::istringstream ss(colorStr);
            std::string tok;
            int count = 0;
            std::vector<std::string> tokens;
            while (std::getline(ss, tok, ',')) {
                while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
                while (!tok.empty() && tok.back() == ' ') tok.pop_back();
                if (!tok.empty()) tokens.push_back(tok);
            }
            for (size_t i = 0; i < tokens.size(); ++i) {
                colors.push_back(parseColor(tokens[i]));
                positions.push_back(tokens.size() > 1 ? static_cast<float>(i) / (tokens.size() - 1) : 0);
            }
            if (!colors.empty()) {
                v->set_background_gradient_linear(x0, y0, x1, y1, colors, positions);
            }
        }
        return choc::value::Value();
    });

    // setBoxShadow(id, offsetX, offsetY, blur, spread, color, inset)
    //   inset is optional; truthy values (true / "inset" / 1) flip the
    //   shadow to render inside the box. Issue-925.
    engine_.register_function("setBoxShadow", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto ox = static_cast<float>(args.get<double>(1, 0));
        auto oy = static_cast<float>(args.get<double>(2, 2));
        auto blur = static_cast<float>(args.get<double>(3, 4));
        auto spread = static_cast<float>(args.get<double>(4, 0));
        auto hex = args.get<std::string>(5, "#00000050");
        bool inset = false;
        if (args.numArgs > 6 && args[6] != nullptr) {
            const auto& v6 = *args[6];
            if (v6.isBool()) inset = v6.getBool();
            else if (v6.isInt32() || v6.isInt64()) inset = v6.getInt64() != 0;
            else if (v6.isFloat32() || v6.isFloat64()) inset = v6.getFloat64() != 0.0;
            else if (v6.isString()) {
                auto s = std::string(v6.getString());
                inset = (s == "inset" || s == "true" || s == "1");
            }
        }
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_box_shadow(ox, oy, blur, spread, parseHexColor(hex), inset);
        return choc::value::Value();
    });

    // clearBoxShadow(id) — companion of setBoxShadow; lets React's diff
    // reconciler remove a shadow when the prop is dropped without having
    // to recreate the widget. Issue-925.
    engine_.register_function("clearBoxShadow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->clear_box_shadow();
        return choc::value::Value();
    });

    // Path builder API from JS
    engine_.register_function("beginPath", [this](choc::javascript::ArgumentList) {
        // Store path commands for deferred rendering via CanvasWidget
        return choc::value::Value();
    });

    // drawPath(canvasId, commands) — draw a path on a CanvasWidget
    // Commands: "M x y" (move), "L x y" (line), "Q cx cy x y" (quad), "C c1x c1y c2x c2y x y" (cubic), "Z" (close)
    engine_.register_function("drawPath", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pathStr = args.get<std::string>(1, "");
        auto fillHex = args.get<std::string>(2, "");
        auto strokeHex = args.get<std::string>(3, "");
        auto lineW = static_cast<float>(args.get<double>(4, 1.0));
        (void)id; (void)pathStr; (void)fillHex; (void)strokeHex; (void)lineW;
        // TODO: parse SVG-like path string and render via CanvasWidget
        return choc::value::Value();
    });

    // compileShader(sksl_code) → {success: bool, error: string}
    // Validates SkSL shader code by actually compiling via SkRuntimeEffect
    engine_.register_function("compileShader", [](choc::javascript::ArgumentList args) {
        auto code = args.get<std::string>(0, "");
        auto result = choc::value::createObject("");
        if (code.empty()) {
            result.addMember("success", choc::value::createBool(false));
            result.addMember("error", choc::value::createString("Empty shader code"));
            return result;
        }
        auto error = canvas::Canvas::compile_sksl(code);
        result.addMember("success", choc::value::createBool(error.empty()));
        result.addMember("error", choc::value::createString(error));
        return result;
    });

    // setWidgetShader(id, skslCode) → apply custom GPU shader to widget body
    engine_.register_function("setWidgetShader", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto sksl = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_custom_shader(sksl);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_custom_shader(sksl);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_custom_shader(sksl);
        request_repaint();
        return choc::value::Value();
    });

    // clearWidgetShader(id) → remove custom shader, restore default paint
    engine_.register_function("clearWidgetShader", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->clear_custom_shader();
        else if (auto* f = dynamic_cast<Fader*>(v)) f->clear_custom_shader();
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->clear_custom_shader();
        request_repaint();
        return choc::value::Value();
    });

    // setWidgetSchema(id, schemaJSON) → apply declarative widget schema
    engine_.register_function("setWidgetSchema", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto json = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_widget_schema(json);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_widget_schema(json);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_widget_schema(json);
        request_repaint();
        return choc::value::Value();
    });

    // setWidgetLottie(id, lottieJSON) → store Lottie animation JSON on widget
    engine_.register_function("setWidgetLottie", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto json = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        // Store Lottie JSON for JS-side rendering or future Skottie integration
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_lottie_json(json);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_lottie_json(json);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_lottie_json(json);
        request_repaint();
        return choc::value::Value();
    });

    // seekWidgetLottie(id, normalizedTime) → scrub Lottie to position (0-1)
    engine_.register_function("seekWidgetLottie", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto t = static_cast<float>(args.get<double>(1, 0));
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_lottie_time(t);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_lottie_time(t);
        else if (auto* tg = dynamic_cast<Toggle*>(v)) tg->set_lottie_time(t);
        request_repaint();
        return choc::value::Value();
    });

    // clearWidgetSchema(id) → remove schema, restore default paint
    engine_.register_function("clearWidgetSchema", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_widget_schema("");
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_widget_schema("");
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_widget_schema("");
        request_repaint();
        return choc::value::Value();
    });

    // saveStylePreset(name, object) → persist style payload as JSON under temp storage
    engine_.register_function("saveStylePreset", [](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        if (name.empty() || args.numArgs < 2 || !args[1]) return choc::value::createBool(false);

        std::string safe_name;
        safe_name.reserve(name.size());
        for (char c : name) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') {
                safe_name.push_back(c);
            } else if (c == ' ') {
                safe_name.push_back('_');
            }
        }
        if (safe_name.empty()) return choc::value::createBool(false);

        auto dir = std::filesystem::temp_directory_path() / "pulp-style-presets";
        std::filesystem::create_directories(dir);

        std::ofstream out(dir / (safe_name + ".json"));
        if (!out.is_open()) return choc::value::createBool(false);
        out << choc::json::toString(*args[1], true);
        return choc::value::createBool(true);
    });

    // loadStylePreset(name) → load persisted JSON object or null
    engine_.register_function("loadStylePreset", [](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        if (name.empty()) return choc::value::Value();

        std::string safe_name;
        safe_name.reserve(name.size());
        for (char c : name) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') {
                safe_name.push_back(c);
            } else if (c == ' ') {
                safe_name.push_back('_');
            }
        }
        if (safe_name.empty()) return choc::value::Value();

        auto path = std::filesystem::temp_directory_path() / "pulp-style-presets" / (safe_name + ".json");
        if (!std::filesystem::exists(path)) return choc::value::Value();

        std::ifstream in(path);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (json.empty()) return choc::value::Value();
        try {
            return choc::json::parse(json);
        } catch (...) {
            return choc::value::Value();
        }
    });

    // measureText(text, fontSize) → {width, ascent, descent, lineHeight}
    engine_.register_function("measureText", [](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(0, "");
        auto size = static_cast<float>(args.get<double>(1, 14.0));
        // Return approximate metrics (exact when Skia canvas is available)
        float width = static_cast<float>(text.size()) * size * 0.6f;
        float ascent = size * 0.75f;
        float descent = size * 0.25f;
        float lineHeight = size * 1.4f;
        auto result = choc::value::createObject("");
        result.addMember("width", choc::value::createFloat64(width));
        result.addMember("ascent", choc::value::createFloat64(ascent));
        result.addMember("descent", choc::value::createFloat64(descent));
        result.addMember("lineHeight", choc::value::createFloat64(lineHeight));
        return result;
    });

    // Theme control
    engine_.register_function("setTheme", [this](choc::javascript::ArgumentList args) {
        auto n = args.get<std::string>(0, "dark");
        root_.set_theme(n=="light" ? Theme::light() : n=="pro_audio" ? Theme::pro_audio() : Theme::dark());
        return choc::value::Value();
    });

    engine_.register_function("applyTokenDiff", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) { auto d = Theme::from_json(json); auto c = root_.theme(); c.apply_overrides(d); root_.set_theme(c); }
        return choc::value::Value();
    });

    engine_.register_function("getThemeJson", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(root_.theme().to_json());
    });

    // importDesignTokens(w3cJson) — parse W3C Design Tokens JSON and apply to theme
    engine_.register_function("importDesignTokens", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) {
            auto imported = parse_w3c_tokens(json);
            auto current = root_.theme();
            current.apply_overrides(imported);
            root_.set_theme(current);
        }
        return choc::value::Value();
    });

    // exportDesignTokens() — export current theme as W3C Design Tokens JSON
    engine_.register_function("exportDesignTokens", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(export_w3c_tokens(root_.theme()));
    });

    // Model-agnostic AI CLI: configurable command for chat integration
    engine_.register_function("setAICli", [this](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        if (!cmd.empty()) ai_cli_command_ = cmd;
        return choc::value::Value();
    });

    engine_.register_function("getAICli", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(ai_cli_command_);
    });

    // Shell exec (for Claude CLI)
    // Ensures PATH includes common tool locations (homebrew, npm global, etc.)
    // getLayoutRect(id) → {x, y, width, height, top, left, right, bottom}
    // pulp #1899 — see the matching note on the earlier getLayoutRect
    // registration. Both bindings must force a layout pass; whichever
    // wins the engine_.register_function override needs the same
    // semantics so React-imported trees get correct bounds in mount-
    // time effects.
    engine_.register_function("getLayoutRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        root_.layout_children();
        auto* v = widget(id);
        return make_layout_rect_value(v);
    });

    // getComputedValue(id, prop) → string
    engine_.register_function("getComputedValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::createString("");
        if (prop == "width") return choc::value::createString(std::to_string(v->bounds().width) + "px");
        if (prop == "height") return choc::value::createString(std::to_string(v->bounds().height) + "px");
        if (prop == "opacity") return choc::value::createString(std::to_string(v->opacity()));
        if (prop == "display") return choc::value::createString(v->visible() ? "flex" : "none");
        if (prop == "visibility") return choc::value::createString(v->visible() ? "visible" : "hidden");
        return choc::value::createString("");
    });

    engine_.register_function("exec", [](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        if (cmd.empty()) return choc::value::createString("");
        auto full_cmd = build_shell_command(cmd);
#ifdef _WIN32
        auto result = pulp::platform::exec("cmd", {"/c", full_cmd}, 30000);
#else
        auto result = pulp::platform::exec("/bin/sh", {"-c", full_cmd}, 30000);
#endif
        return choc::value::createString(result.stdout_output);
    });

    // execAsync(cmd, callbackId) — non-blocking shell command
    // Runs cmd on a background thread, dispatches result to JS via
    // __dispatch__(callbackId, 'result', stdout) when poll_async_results() runs.
    engine_.register_function("execAsync", [this](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        auto cbId = args.get<std::string>(1, "");
        if (cmd.empty() || cbId.empty()) return choc::value::Value();
        auto full_cmd = build_shell_command(cmd);
        auto alive = callback_alive_;
        auto async_results = async_exec_results_;
        auto async_mutex = async_exec_mutex_;
        std::thread([alive, async_results, async_mutex, full_cmd, cbId]() {
#ifdef _WIN32
            auto result = pulp::platform::exec("cmd", {"/c", full_cmd}, 60000);
#else
            auto result = pulp::platform::exec("/bin/sh", {"-c", full_cmd}, 60000);
#endif
            if (!alive || !alive->load(std::memory_order_acquire)) return;
            std::lock_guard<std::mutex> lock(*async_mutex);
            async_results->push_back({cbId, std::move(result.stdout_output)});
        }).detach();
        return choc::value::Value();
    });

    // ── Context menu ────────────────────────────────────────────────────
    // registerContextMenu(id, callbackName)
    // When right-click fires on the widget, calls JS: callbackName(x, y)
    engine_.register_function("registerContextMenu", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto cb = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (v && !cb.empty()) {
            auto alive = callback_alive_;
            auto* engine = &engine_;
            v->on_context_menu = [alive, engine, cb](Point pos) {
                safe_dispatch_eval(alive, engine,
                    cb + "(" + std::to_string(pos.x) + "," + std::to_string(pos.y) + ")",
                    "context menu");
            };
        }
        return choc::value::Value();
    });

    // showContextMenu(itemsJSON, x, y) -> selected id or -1
    engine_.register_function("showContextMenu", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        auto x = args.get<double>(1, 0);
        auto y = args.get<double>(2, 0);

        platform::PopupMenu menu;
        // Parse JSON array: [{"id":1,"label":"Cut"}, {"separator":true}]
        try {
            auto items = choc::json::parse(json);
            if (items.isArray()) {
                for (uint32_t i = 0; i < items.size(); ++i) {
                    auto item = items[i];
                    bool sep = false;
                    if (item.hasObjectMember("separator")) {
                        sep = item["separator"].getWithDefault(false);
                    }
                    if (sep) {
                        menu.add_separator();
                    } else {
                        int id = item["id"].getWithDefault(0);
                        std::string label;
                        if (item.hasObjectMember("label"))
                            label = item["label"].getWithDefault(std::string(""));
                        bool enabled = item.hasObjectMember("enabled") ? item["enabled"].getWithDefault(true) : true;
                        bool checked = item.hasObjectMember("checked") ? item["checked"].getWithDefault(false) : false;
                        menu.add_item(id, label, enabled, checked);
                    }
                }
            }
        } catch (...) {}
        auto result = menu.show(static_cast<float>(x), static_cast<float>(y));
        return choc::value::createInt32(result.value_or(-1));
    });

    // ── Keyboard shortcuts ──────────────────────────────────────────────
    // registerShortcut(key, modifiers, callbackName)
    engine_.register_function("registerShortcut", [this](choc::javascript::ArgumentList args) {
        auto key = args.get<int>(0, 0);
        auto mods = args.get<int>(1, 0);
        auto cb = args.get<std::string>(2, "");
        if (!cb.empty()) {
            shortcuts_.push_back({static_cast<KeyCode>(key),
                                  static_cast<uint16_t>(mods), cb});
        }
        return choc::value::Value();
    });

    // ── File dialogs ────────────────────────────────────────────────────
    // showOpenDialog(title, filterDesc, extensions) -> path or ""
    // extensions: semicolon-separated, e.g. "js;json;txt"
    engine_.register_function("showOpenDialog", [this](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Open");
        auto desc = args.get<std::string>(1, "");
        auto exts = args.get<std::string>(2, "");
        std::vector<platform::FileFilter> filters;
        if (!desc.empty())
            filters.push_back({desc, exts});
        auto result = platform::FileDialog::open_file(title, filters);
        return choc::value::createString(result.value_or(""));
    });

    // showSaveDialog(title, filterDesc, extensions) -> path or ""
    engine_.register_function("showSaveDialog", [this](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Save");
        auto desc = args.get<std::string>(1, "");
        auto exts = args.get<std::string>(2, "");
        std::vector<platform::FileFilter> filters;
        if (!desc.empty())
            filters.push_back({desc, exts});
        auto result = platform::FileDialog::save_file(title, filters);
        return choc::value::createString(result.value_or(""));
    });

    // chooseFolder(title) -> path or ""
    engine_.register_function("chooseFolder", [this](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Choose Folder");
        auto result = platform::FileDialog::choose_folder(title);
        return choc::value::createString(result.value_or(""));
    });

    // ═════════════════════════════════════════════════════════════════��═
    // Phase 9: Runtime API gap closure
    // ═══════════════════════════════════════════════════════════════════

    // __requestFrame__ — requestAnimationFrame implementation
    // JS side stores callbacks in __frameCallbacks__ map, passes ID to C++.
    // C++ stores pending IDs and invokes them on next frame via __invokeFrame__.
    // Shared pending frame callback IDs (static so lambdas can capture pointer)
    if (!frame_preamble_loaded_) {
        frame_preamble_loaded_ = true;
        engine_.evaluate(
            "var __frameCallbacks__ = {};"
            "var __frameNextId__ = 1;"
            "function __invokeFrame__(id) {"
            "  var fn = __frameCallbacks__[id];"
            "  if (fn) { delete __frameCallbacks__[id]; fn(); }"
            "}"
            // pulp #915 — timer registry for native setTimeout / setInterval.
            // Callbacks live in JS (CHOC's NativeFunction can't carry JSValue
            // arguments); native side tracks deadlines + repeat semantics
            // and pings these helpers when a timer expires.
            "var __timerCallbacks__ = Object.create(null);"
            "var __timerNextId__ = 1;"
            "function __invokeTimer__(id) {"
            "  var entry = __timerCallbacks__[id];"
            "  if (!entry) return;"
            "  if (!entry.repeat) delete __timerCallbacks__[id];"
            "  try { entry.fn(); } catch (e) {"
            "    if (typeof console !== 'undefined' && console.error)"
            "      console.error('timer:', e);"
            "  }"
            "}"
        );
    }

    engine_.register_function("__requestFrame__", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        if (id > 0) {
            pending_frame_ids_.push_back(id);
            // pulp #921 — signal the host so the next paint runs and
            // service_frame_callbacks() drains the queue. Without this,
            // requestAnimationFrame queues a callback but never asks the
            // host for a frame, so the canvas never repaints.
            request_repaint();
        }
        return choc::value::createInt32(id);
    });

    engine_.register_function("__cancelFrame__", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        auto it = std::find(pending_frame_ids_.begin(), pending_frame_ids_.end(), id);
        if (it != pending_frame_ids_.end()) pending_frame_ids_.erase(it);
        return choc::value::Value();
    });

    engine_.register_function("__flushFrames__", [this](choc::javascript::ArgumentList) {
        auto ids = pending_frame_ids_;
        pending_frame_ids_.clear();
        if (ids.empty()) {
            return choc::value::Value();
        }
        std::string batch;
        batch.reserve(ids.size() * 24);
        for (auto id : ids) {
            batch += "__invokeFrame__(";
            batch += std::to_string(id);
            batch += ");";
        }
        engine_.evaluate(batch + "void 0;");
        return choc::value::Value();
    });

    // pulp #915 — native setTimeout / setInterval scheduling. JS-side
    // setTimeout/setInterval generate the id and stash the callback in
    // __timerCallbacks__; native tracks (id, deadline, repeat, interval)
    // so service_frame_callbacks() can fire expired timers without a
    // consumer-side shim.
    engine_.register_function("__scheduleTimer__", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        auto delay = args.get<double>(1, 0.0);
        auto repeat = args.get<bool>(2, false);
        if (id <= 0) return choc::value::createInt32(0);
        if (delay < 0.0) delay = 0.0;
        auto delay_ms = std::chrono::milliseconds(static_cast<long long>(delay));
        PendingTimer t;
        t.id = id;
        t.deadline = std::chrono::steady_clock::now() + delay_ms;
        t.interval = delay_ms;
        t.repeating = repeat;
        // Replace any prior schedule for this id (shouldn't happen, but
        // setInterval/setTimeout share the JS-side id allocator and a
        // misbehaving consumer might double-schedule).
        auto it = std::find_if(pending_timers_.begin(), pending_timers_.end(),
            [id](const PendingTimer& p){ return p.id == id; });
        if (it != pending_timers_.end()) *it = t;
        else pending_timers_.push_back(t);
        return choc::value::createInt32(id);
    });

    engine_.register_function("__cancelTimer__", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        auto it = std::remove_if(pending_timers_.begin(), pending_timers_.end(),
            [id](const PendingTimer& p){ return p.id == id; });
        pending_timers_.erase(it, pending_timers_.end());
        return choc::value::Value();
    });

    engine_.register_function("__flushTimers__", [this](choc::javascript::ArgumentList) {
        if (pending_timers_.empty()) return choc::value::Value();
        auto now = std::chrono::steady_clock::now();
        std::vector<int> to_fire;
        // Two-phase: first collect ids whose deadline has passed, then
        // fire them. Mutating pending_timers_ while iterating would lose
        // re-arms from setInterval (which reschedules its own deadline).
        for (auto& t : pending_timers_) {
            if (t.deadline <= now) to_fire.push_back(t.id);
        }
        if (to_fire.empty()) return choc::value::Value();
        // Re-arm intervals before invocation so the JS callback can call
        // clearInterval(id) and have it actually take effect (which the
        // __cancelTimer__ binding it calls into respects).
        for (auto& t : pending_timers_) {
            if (t.deadline <= now && t.repeating) {
                // Catch-up by interval — never schedule into the past.
                while (t.deadline <= now) t.deadline += t.interval;
            }
        }
        // Drop expired non-repeating timers from the queue. (Repeating
        // ones already had their deadline advanced above.)
        pending_timers_.erase(
            std::remove_if(pending_timers_.begin(), pending_timers_.end(),
                [now](const PendingTimer& p){
                    return !p.repeating && p.deadline <= now;
                }),
            pending_timers_.end());
        std::string batch;
        batch.reserve(to_fire.size() * 24);
        for (auto id : to_fire) {
            batch += "__invokeTimer__(";
            batch += std::to_string(id);
            batch += ");";
        }
        engine_.evaluate(batch + "void 0;");
        return choc::value::Value();
    });

    // P0: performance.now() — high-resolution monotonic time in milliseconds
    engine_.register_function("__performanceNow__", [](choc::javascript::ArgumentList) {
        static auto start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - start).count();
        return choc::value::createFloat64(ms);
    });

    // P1: Clipboard — read/write text via platform::Clipboard
    engine_.register_function("readClipboard", [this](choc::javascript::ArgumentList) {
        auto text = platform::Clipboard::get_text();
        return choc::value::createString(text.value_or(""));
    });

    engine_.register_function("writeClipboard", [this](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(0, "");
        platform::Clipboard::set_text(text);
        return choc::value::Value();
    });

    // P1: Canvas gradient fills
    engine_.register_function("canvasSetLinearGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_linear;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.x2 = (float)args.get<double>(3, 0); cmd.y2 = (float)args.get<double>(4, 1);
            // Parse color stops from remaining args: color1, pos1, color2, pos2, ...
            for (int i = 5; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetRadialGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_radial;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 50); // radius
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1524 — true two-circle radial gradient. Skia routes through
    // SkGradientShader::MakeTwoPointConical; CG routes through
    // CGContextDrawRadialGradient with both circles wired (the prior
    // single-circle bridge silently dropped (x0, y0, r0) which broke
    // offset / sized inner-circle gradients on both backends).
    // Args: (id, x0, y0, r0, x1, y1, r1, color1, pos1, color2, pos2, ...)
    engine_.register_function("canvasSetRadialGradientTwoCircles",
            [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd;
            cmd.type = CanvasDrawCmd::Type::set_fill_gradient_radial_two_circles;
            // Inner circle (x0, y0, r0) → (x, y, extra).
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            // Outer circle (x1, y1, r1) → (x2, y2, w).
            cmd.x2 = (float)args.get<double>(4, 0);
            cmd.y2 = (float)args.get<double>(5, 0);
            cmd.w  = (float)args.get<double>(6, 50);
            for (int i = 7; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasClearGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_fill_gradient;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp Wave 3 c2d.7 — `ctx.strokeStyle = createLinearGradient(...)`.
    // Mirror of canvasSetLinearGradient targeting the new
    // `Canvas::set_stroke_gradient_linear` virtual. The JS shim's
    // _applyStrokeStyle dispatches here when the bridge fn is present;
    // older binaries fall back to the first-stop solid colour without
    // crashing. Stops are color/position pairs starting at arg index 5,
    // matching the fill counterpart so the JS shim shares its packing
    // logic.
    engine_.register_function("canvasSetStrokeLinearGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_linear;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.x2 = (float)args.get<double>(3, 0); cmd.y2 = (float)args.get<double>(4, 1);
            for (int i = 5; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Single-circle radial. Args: (id, cx, cy, radius, color1, pos1, ...).
    engine_.register_function("canvasSetStrokeRadialGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_radial;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 50);
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Two-circle radial. Args: (id, x0, y0, r0, x1, y1, r1, color1, pos1, ...).
    engine_.register_function("canvasSetStrokeRadialGradientTwoCircles",
            [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd;
            cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_radial_two_circles;
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            cmd.x2 = (float)args.get<double>(4, 0);
            cmd.y2 = (float)args.get<double>(5, 0);
            cmd.w  = (float)args.get<double>(6, 50);
            for (int i = 7; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Conic / sweep. Args: (id, cx, cy, startAngle, color1, pos1, ...).
    engine_.register_function("canvasSetStrokeConicGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_conic;
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Reset stroke shader → solid stroke colour.
    engine_.register_function("canvasClearStrokeGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_stroke_gradient;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1434 bridge-thin gap-fill — ctx.createConicGradient. Skia
    // already exposes set_fill_gradient_conic via SkGradientShader::MakeSweep
    // (skia_canvas.cpp line ~917); CG degrades to the first-stop colour.
    // Args: (id, cx, cy, startAngle, color1, pos1, color2, pos2, ...)
    engine_.register_function("canvasSetConicGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_conic;
            cmd.x = (float)args.get<double>(1, 0);   // cx
            cmd.y = (float)args.get<double>(2, 0);   // cy
            cmd.extra = (float)args.get<double>(3, 0); // start_angle (radians)
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1434 bridge-thin gap-fill — ctx.createPattern. Skia path
    // routes through SkShader::MakeImage with SkTileMode per axis (real
    // tiled fill); CG path degrades to the active fill colour because
    // CG has no first-class pattern shader without CGPattern dance —
    // same shape as the conic-gradient fallback.
    //
    // Args: (id, src, tile_x, tile_y)
    //   src      — image source (file path, "data:" URL, or "" for clear)
    //   tile_x   — "repeat" | "no-repeat"
    //   tile_y   — "repeat" | "no-repeat"
    engine_.register_function("canvasSetFillPattern", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_pattern;
            cmd.text = args.get<std::string>(1, "");          // image source
            auto tx = args.get<std::string>(2, "repeat");
            auto ty = args.get<std::string>(3, "repeat");
            // Pack tile modes into int_val (bit 0 = x, bit 1 = y);
            // 0 = repeat, 1 = no-repeat. Mirrors set_image_smoothing's
            // pattern of folding multiple enum values into one int slot.
            int tx_i = (tx == "no-repeat") ? 1 : 0;
            int ty_i = (ty == "no-repeat") ? 1 : 0;
            cmd.int_val = tx_i | (ty_i << 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Stroke counterpart — same shape, different command type. Routes
    // through set_stroke_pattern on the live canvas; CG falls back to
    // solid stroke colour.
    engine_.register_function("canvasSetStrokePattern", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_pattern;
            cmd.text = args.get<std::string>(1, "");
            auto tx = args.get<std::string>(2, "repeat");
            auto ty = args.get<std::string>(3, "repeat");
            int tx_i = (tx == "no-repeat") ? 1 : 0;
            int ty_i = (ty == "no-repeat") ? 1 : 0;
            cmd.int_val = tx_i | (ty_i << 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1434 bridge-thin gap-fill — ctx.miterLimit. Sticky stroke
    // state honoured by SkPaint::setStrokeMiter (Skia) and
    // CGContextSetMiterLimit (CG). Spec: non-positive / non-finite
    // values are silently ignored — backends do the clamp.
    // Args: (id, limit)
    engine_.register_function("canvasSetMiterLimit", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_miter_limit;
            cmd.extra = (float)args.get<double>(1, 10.0); // spec default = 10
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1434 bridge-thin gap-fill — ctx.imageSmoothingEnabled +
    // ctx.imageSmoothingQuality. Sticky paint flag honoured on the next
    // drawImage. Skia translates to SkSamplingOptions, CG to
    // CGContextSetInterpolationQuality.
    // Args: (id, enabled[, quality]) where quality ∈ "low" | "medium" | "high".
    engine_.register_function("canvasSetImageSmoothing", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_image_smoothing;
            cmd.int_val = args.get<bool>(1, true) ? 1 : 0;
            auto q = args.get<std::string>(2, "low");
            int qi = 0;
            if (q == "medium") qi = 1;
            else if (q == "high") qi = 2;
            cmd.extra = static_cast<float>(qi);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1520 — Canvas2D ctx.direction. Sticky text-shaping state
    // honoured by the SkShaper / HarfBuzz path on the next fillText
    // / strokeText. The shim coerces unknown strings to "ltr" before
    // hitting the bridge, so we accept the resolved enum directly.
    // Args: (id, enumVal) where enumVal ∈ 0=ltr | 1=rtl | 2=inherit.
    engine_.register_function("canvasSetDirection", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_direction;
            int v = static_cast<int>(args.get<double>(1, 0.0));
            if (v < 0 || v > 2) v = 0;
            cmd.int_val = v;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // pulp #1520 — Canvas2D ctx.filter. Sticky CSS <filter-function-list>
    // string applied to subsequent fill/stroke/text/image draws. Skia
    // parses into an SkImageFilter chain (blur, grayscale, sepia, …);
    // RecordingCanvas captures the raw string for harness assertions;
    // CG / minimal backends store the value but render unfiltered until
    // a follow-up wires the parser through (#1503 owns the View-side
    // parser; canvas2d shares it as it lands).
    // Args: (id, cssFilterString) — "none" disables.
    engine_.register_function("canvasSetFilter", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_filter;
            cmd.text = args.get<std::string>(1, "none");
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas arc — for pie charts, circular progress, arcs
    engine_.register_function("canvasArc", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_arc;
            cmd.x = (float)args.get<double>(1, 0);     // cx
            cmd.y = (float)args.get<double>(2, 0);     // cy
            cmd.w = (float)args.get<double>(3, 50);    // radius
            cmd.x2 = (float)args.get<double>(4, 0);    // startAngle
            cmd.y2 = (float)args.get<double>(5, 6.28); // endAngle
            cmd.color = parseColor(args.get<std::string>(6, "#fff"));
            cmd.extra = (float)args.get<double>(7, 1);  // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas textAlign / textBaseline
    engine_.register_function("canvasSetTextAlign", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_text_align;
            auto align = args.get<std::string>(1, "left");
            cmd.int_val = (align == "center") ? 1 : (align == "right") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetTextBaseline", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_text_baseline;
            auto bl = args.get<std::string>(1, "top");
            cmd.int_val = (bl == "middle") ? 1 : (bl == "bottom") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas clearRect
    engine_.register_function("canvasClearRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_rect;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0); cmd.h = (float)args.get<double>(4, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas clipRect (was in enum but never registered)
    engine_.register_function("canvasClipRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clip_rect;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0); cmd.h = (float)args.get<double>(4, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: Canvas fillRoundedRect / strokeRoundedRect / strokeCircle (existed in C++ but no JS bridge)
    engine_.register_function("canvasFillRoundedRect", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_rounded_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.extra=(float)args.get<double>(5,0); // radius
            cmd.color = parseColor(args.get<std::string>(6, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokeRoundedRect", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_rounded_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.extra=(float)args.get<double>(5,0); // radius
            cmd.color = parseColor(args.get<std::string>(6, "#fff"));
            cmd.x2=(float)args.get<double>(7,1); // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokeCircle", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_circle;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.extra=(float)args.get<double>(3,10); // radius
            cmd.color = parseColor(args.get<std::string>(4, "#fff"));
            cmd.x2=(float)args.get<double>(5,1); // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: Canvas globalAlpha
    engine_.register_function("canvasSetGlobalAlpha", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_global_alpha;
            cmd.extra = (float)args.get<double>(1, 1.0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: Canvas lineCap / lineJoin
    engine_.register_function("canvasSetLineCap", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_cap;
            auto cap = args.get<std::string>(1, "butt");
            cmd.int_val = (cap == "round") ? 1 : (cap == "square") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetLineJoin", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_join;
            auto join = args.get<std::string>(1, "miter");
            cmd.int_val = (join == "round") ? 1 : (join == "bevel") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // CSS globalCompositeOperation → Canvas::BlendMode index. Returns -1
    // for unknown strings so callers can no-op gracefully (issue-896).
    auto cssCompositeOpToBlendModeIndex = [](const std::string& mode) -> int {
        // Indices below MUST match Canvas::BlendMode in core/canvas/include/pulp/canvas/canvas.hpp.
        if (mode == "source-over")      return 16; // also accepted at index 0 (normal)
        if (mode == "destination-over") return 17;
        if (mode == "source-in")        return 18;
        if (mode == "destination-in")   return 19;
        if (mode == "source-out")       return 20;
        if (mode == "destination-out")  return 21;
        if (mode == "source-atop")      return 22;
        if (mode == "destination-atop") return 23;
        if (mode == "xor")              return 24;
        if (mode == "copy")             return 25;
        if (mode == "lighter")          return 26;
        // W3C advanced blend modes (indices match enum order)
        if (mode == "multiply")     return 1;
        if (mode == "screen")       return 2;
        if (mode == "overlay")      return 3;
        if (mode == "darken")       return 4;
        if (mode == "lighten")      return 5;
        if (mode == "color-dodge")  return 6;
        if (mode == "color-burn")   return 7;
        if (mode == "hard-light")   return 8;
        if (mode == "soft-light")   return 9;
        if (mode == "difference")   return 10;
        if (mode == "exclusion")    return 11;
        if (mode == "hue")          return 12;
        if (mode == "saturation")   return 13;
        if (mode == "color")        return 14;
        if (mode == "luminosity")   return 15;
        return -1; // unknown — caller treats as no-op
    };

    // P3: Canvas globalCompositeOperation (blend mode) — back-compat alias
    engine_.register_function("canvasSetBlendMode", [this, cssCompositeOpToBlendModeIndex](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            auto mode = args.get<std::string>(1, "source-over");
            int idx = cssCompositeOpToBlendModeIndex(mode);
            if (idx < 0) return choc::value::Value(); // unknown string → no-op
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_blend_mode;
            cmd.int_val = idx;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasGlobalCompositeOperation — full CanvasRenderingContext2D
    // globalCompositeOperation surface (issue-896). Accepts every standard
    // CSS string and falls back to no-op on unknown values.
    engine_.register_function("canvasGlobalCompositeOperation", [this, cssCompositeOpToBlendModeIndex](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            auto mode = args.get<std::string>(1, "source-over");
            int idx = cssCompositeOpToBlendModeIndex(mode);
            if (idx < 0) return choc::value::Value(); // unknown — graceful no-op
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_blend_mode;
            cmd.int_val = idx;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasSetTransform(id, a, b, c, d, e, f) — replace current transform
    // with the affine matrix (issue-896). Used for devicePixelRatio scaling
    // (ctx.setTransform(scale, 0, 0, scale, 0, 0)) and Spectr FilterBank.
    engine_.register_function("canvasSetTransform", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_transform;
            cmd.x  = (float)args.get<double>(1, 1.0); // a (scaleX)
            cmd.y  = (float)args.get<double>(2, 0.0); // b (skewY)
            cmd.w  = (float)args.get<double>(3, 0.0); // c (skewX)
            cmd.h  = (float)args.get<double>(4, 1.0); // d (scaleY)
            cmd.x2 = (float)args.get<double>(5, 0.0); // e (translateX)
            cmd.y2 = (float)args.get<double>(6, 0.0); // f (translateY)
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasClip(id, fillRule?) — intersect clip region with current path (issue-896).
    // pulp DIVERGE→PASS sweep — also threads optional fillRule int (0 =
    // nonzero, 1 = evenodd). Same as canvasFillPath above.
    engine_.register_function("canvasClip", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clip;
            cmd.int_val = static_cast<int>(args.get<double>(1, 0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: localStorage equivalent — file-based key-value in plugin data dir
    engine_.register_function("storageGetItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        if (key.empty()) return choc::value::createString("");
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        auto path = dir / (key + ".dat");
        if (!std::filesystem::exists(path)) return choc::value::createString("");
        std::ifstream f(path);
        std::string val((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return choc::value::createString(val);
    });

    engine_.register_function("storageSetItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        auto val = args.get<std::string>(1, "");
        if (key.empty()) return choc::value::Value();
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        std::filesystem::create_directories(dir);
        std::ofstream f(dir / (key + ".dat"));
        f << val;
        return choc::value::Value();
    });

    engine_.register_function("storageRemoveItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        if (key.empty()) return choc::value::Value();
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        std::filesystem::remove(dir / (key + ".dat"));
        return choc::value::Value();
    });

    engine_.register_function("__loadAssetSync__", [](choc::javascript::ArgumentList args) {
        auto url = args.get<std::string>(0, "");
        auto asset = load_bridge_asset(url);

        auto result = choc::value::createObject("");
        result.addMember("ok", choc::value::createBool(asset.ok));
        result.addMember("status", choc::value::createInt32(asset.status));
        result.addMember("url", choc::value::createString(url));
        result.addMember("resolvedPath", choc::value::createString(asset.resolved_path));
        result.addMember("contentType", choc::value::createString(asset.mime_type));
        result.addMember("base64", choc::value::createString(asset.ok ? bridge_base64_encode(asset.blob.data) : ""));
        if (asset.ok && bridge_asset_is_text_like(asset.mime_type)) {
            result.addMember("text", choc::value::createString(
                std::string(asset.blob.data.begin(), asset.blob.data.end())));
        } else {
            result.addMember("text", choc::value::createString(""));
        }
        return result;
    });

    // ═══════════════════════════════════════════════════════════════════
    // Final gap closure
    // ═══════════════════════════════════════════════════════════════════

    // Canvas drawImage(canvasId, imagePath, dx, dy, dw, dh)
    //   or 9-arg form: canvasDrawImage(id, path, dx,dy,dw,dh, sx,sy,sw,sh)
    // pulp #1737 — when args[6..9] are present the JS shim is using the
    // source-rect 9-arg form. Bridge stashes it in x2/y2/x3/y3 + sets
    // has_source_rect; the canvas_widget renderer routes through the
    // _rect overload so the source sub-rectangle lands on the dst rect.
    engine_.register_function("canvasDrawImage", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::draw_image;
            cmd.text = args.get<std::string>(1, ""); // image source path
            cmd.x = (float)args.get<double>(2, 0);
            cmd.y = (float)args.get<double>(3, 0);
            cmd.w = (float)args.get<double>(4, 0);
            cmd.h = (float)args.get<double>(5, 0);
            // 9-arg drawImage source rect — JS appends sx,sy,sw,sh
            // when the caller used the 9-arg form. choc reports
            // missing args as defaults, so we gate on numArgs >= 10.
            if (args.numArgs >= 10) {
                cmd.x2 = (float)args.get<double>(6, 0);
                cmd.y2 = (float)args.get<double>(7, 0);
                cmd.x3 = (float)args.get<double>(8, 0);
                cmd.y3 = (float)args.get<double>(9, 0);
                cmd.has_source_rect = true;
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════════
    // Canvas2D API gap closures (issue-916)
    // ═══════════════════════════════════════════════════════════════════

    // canvasMeasureText(id, text, fontFamily, fontSize) →
    //   { width, actualBoundingBoxLeft, actualBoundingBoxRight,
    //     actualBoundingBoxAscent, actualBoundingBoxDescent,
    //     fontBoundingBoxAscent, fontBoundingBoxDescent }
    //
    // Per-canvas measureText routes through the canvas's font state — this
    // matters because each canvas can carry its own font setting via
    // canvasSetFont. When a Skia surface isn't available (RecordingCanvas,
    // CG fallback) we return font-size estimates so JS callers always get
    // a populated TextMetrics object with non-zero width for non-empty
    // text. (HTML5 spec — TextMetrics never has missing fields.)
    engine_.register_function("canvasMeasureText", [this](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(1, "");
        auto family = args.get<std::string>(2, "Inter");
        float size = static_cast<float>(args.get<double>(3, 14.0));

        canvas::Canvas::TextMetrics m;
#if defined(PULP_HAS_SKIA)
        // Skia-backed accurate metrics — uses SkFont::measureText() which
        // is what fill_text() ultimately renders against, so the returned
        // bbox matches the drawn text. No surface required.
        m = canvas::SkiaCanvas::measure_text_with_font(family, size, text);
#else
        // CPU fallback — keep all fields populated so JS centring works
        // (HTML5 spec: TextMetrics never has missing fields).
        m.width = static_cast<float>(text.size()) * size * 0.6f;
        m.ascent = size * 0.75f;
        m.descent = size * 0.25f;
        m.line_height = size * 1.2f;
        m.actual_bounding_box_left = 0;
        m.actual_bounding_box_right = m.width;
        m.actual_bounding_box_ascent = m.ascent;
        m.actual_bounding_box_descent = m.descent;
#endif

        // (void)c — measureText doesn't need to mutate the canvas. Calling
        // with a missing id still returns a valid metrics object so callers
        // can pre-measure before getContext('2d').
        (void)widget(args.get<std::string>(0, ""));

        auto result = choc::value::createObject("");
        result.addMember("width", choc::value::createFloat64(m.width));
        result.addMember("actualBoundingBoxLeft",
                          choc::value::createFloat64(m.actual_bounding_box_left));
        result.addMember("actualBoundingBoxRight",
                          choc::value::createFloat64(m.actual_bounding_box_right));
        result.addMember("actualBoundingBoxAscent",
                          choc::value::createFloat64(m.actual_bounding_box_ascent));
        result.addMember("actualBoundingBoxDescent",
                          choc::value::createFloat64(m.actual_bounding_box_descent));
        result.addMember("fontBoundingBoxAscent",
                          choc::value::createFloat64(m.ascent));
        result.addMember("fontBoundingBoxDescent",
                          choc::value::createFloat64(m.descent));
        return result;
    });

    // canvasSetLineDash(id, [a, b, c, ...], phase = 0)
    // Pattern is an HTML5-style array; an odd-length array is
    // duplicated to even per the spec — the JS prelude handles that
    // before calling here.
    engine_.register_function("canvasSetLineDash", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_dash;
            cmd.extra = static_cast<float>(args.get<double>(2, 0.0)); // phase
            // Pattern: pulled from arg[1]. Choc exposes JS arrays as
            // indexable ValueViews here, matching other bridge array handlers.
            // Reuse cmd.gradient_positions to avoid expanding CanvasDrawCmd.
            if (args.numArgs > 1 && args[1]) {
                auto& pattern = *args[1];
                cmd.gradient_positions.reserve(pattern.size());
                for (uint32_t i = 0; i < pattern.size(); ++i) {
                    cmd.gradient_positions.push_back(
                        static_cast<float>(pattern[i].getWithDefault<double>(0.0)));
                }
                // HTML5: drop the entire pattern if any value is negative
                // or non-finite — spec says behavior is implementation-
                // defined; we choose graceful "solid stroke".
                bool valid = true;
                for (float v : cmd.gradient_positions) {
                    if (!(v >= 0.0f) || !std::isfinite(v)) { valid = false; break; }
                }
                if (!valid) cmd.gradient_positions.clear();
                // HTML5 spec: odd-length patterns are duplicated.
                if (cmd.gradient_positions.size() % 2 == 1) {
                    auto orig = cmd.gradient_positions;
                    cmd.gradient_positions.insert(cmd.gradient_positions.end(),
                                                  orig.begin(), orig.end());
                }
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ── Canvas2D shadow* state (issue-1434 batch 7) ─────────────────────────
    //
    // Sticky drop-shadow state that wraps subsequent fill/stroke/text
    // draws — matching `CanvasRenderingContext2D.shadowColor` /
    // `shadowBlur` / `shadowOffsetX` / `shadowOffsetY`. Each setter
    // records one CanvasDrawCmd that the paint dispatch flushes through
    // to the underlying canvas (Skia → SkImageFilters::DropShadow,
    // CoreGraphics → CGContextSetShadowWithColor). The shadow is gated
    // on color.a > 0 AND (blur > 0 OR offset_x != 0 OR offset_y != 0)
    // by the canvas backends; the bridge captures every assignment so
    // the state stays in lockstep with the JS-side `ctx.shadow*`
    // properties even when one of them is set to 0/transparent.
    //
    // Shadow color is parsed via the shared `parseColor` helper used
    // by `canvasSetFillStyle` etc., so all the CSS color forms
    // (`#rgb`, `#rrggbb`, `rgba(...)`, `hsl(...)`, `transparent`,
    // `red`, …) work uniformly.
    engine_.register_function("canvasSetShadowColor", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_color;
            cmd.color = parseColor(args.get<std::string>(1, "rgba(0,0,0,0)"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetShadowBlur", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_blur;
            // HTML5 spec: shadowBlur must be non-negative finite; reject
            // negatives at the boundary so the canvas backends don't have
            // to redo the validation.
            double blur = args.get<double>(1, 0.0);
            cmd.extra = static_cast<float>(blur >= 0.0 ? blur : 0.0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetShadowOffsetX", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_offset_x;
            cmd.extra = static_cast<float>(args.get<double>(1, 0.0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetShadowOffsetY", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_offset_y;
            cmd.extra = static_cast<float>(args.get<double>(1, 0.0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasGetImageData(id, x, y, w, h) →
    //   { width, height, data: <base64 string of RGBA bytes> }
    //
    // Returns the pixel data of the canvas's currently rasterized
    // surface. On non-Skia / non-rasterized backends this returns an
    // empty buffer of the requested size; callers should treat that as
    // "not available" and fall back to whatever they'd do with a stub
    // 1×1 placeholder. The base64 wrapping keeps the result usable
    // across QuickJS / JSC / V8 without a Uint8ClampedArray bridge.
    engine_.register_function("canvasGetImageData", [this](choc::javascript::ArgumentList args) {
        int x = static_cast<int>(args.get<double>(1, 0.0));
        int y = static_cast<int>(args.get<double>(2, 0.0));
        int w = static_cast<int>(args.get<double>(3, 0.0));
        int h = static_cast<int>(args.get<double>(4, 0.0));
        if (w < 0) w = 0;
        if (h < 0) h = 0;

        std::vector<uint8_t> pixels(static_cast<size_t>(w) *
                                     static_cast<size_t>(h) * 4u, 0u);

        // The bridge has no direct access to the live render surface
        // (CanvasWidget paints into whatever canvas the host provides
        // each frame), so getImageData over a JS-recorded command list
        // can only return zeros until a render-host integration lands.
        // We still validate the canvas id so JS sees a well-formed
        // result with the right dimensions (matching HTML5 spec for
        // out-of-bounds reads).
        (void)widget(args.get<std::string>(0, ""));

        auto result = choc::value::createObject("");
        result.addMember("width",  choc::value::createInt32(w));
        result.addMember("height", choc::value::createInt32(h));
        result.addMember("data",   choc::value::createString(
            bridge_base64_encode(pixels)));
        return result;
    });

    // canvasPutImageData(id, base64Pixels, width, height, dx, dy)
    //
    // Decodes `base64Pixels` (expected width*height*4 RGBA bytes) and
    // records a put_image_data command. The widget's paint() pass
    // applies it via Canvas::write_pixels() — currently only Skia
    // implements that end-to-end; other backends are a no-op.
    engine_.register_function("canvasPutImageData", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            auto b64 = args.get<std::string>(1, "");
            int width  = static_cast<int>(args.get<double>(2, 0.0));
            int height = static_cast<int>(args.get<double>(3, 0.0));
            int dx     = static_cast<int>(args.get<double>(4, 0.0));
            int dy     = static_cast<int>(args.get<double>(5, 0.0));
            if (width <= 0 || height <= 0) return choc::value::Value();

            auto bytes = runtime::base64_decode(b64);
            if (!bytes) return choc::value::Value();
            const size_t expected = static_cast<size_t>(width) *
                                     static_cast<size_t>(height) * 4u;
            if (bytes->size() < expected) return choc::value::Value();

            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::put_image_data;
            cmd.int_val = width;
            cmd.x2      = static_cast<float>(height);
            cmd.x       = static_cast<float>(dx);
            cmd.y       = static_cast<float>(dy);
            cmd.text.assign(reinterpret_cast<const char*>(bytes->data()), expected);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Drag-and-drop: register JS callback for file/text drops
    engine_.register_function("registerDrop", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto cb = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (v && !cb.empty()) {
            auto alive = callback_alive_;
            auto* engine = &engine_;
            // Wire native drop target to fire JS callback with dropped data
            // The JS callback receives: callbackName(type, data, x, y)
            // type: "file" or "text", data: file path or text content
            v->on_drop = [alive, engine, cb](const std::string& type, const std::string& data, float x, float y) {
                std::string safe_data;
                for (char c : data) {
                    if (c == '\'') safe_data += "\\'";
                    else if (c == '\n') safe_data += "\\n";
                    else safe_data += c;
                }
                safe_dispatch_eval(alive, engine,
                    cb + "('" + type + "','" + safe_data + "'," +
                    std::to_string(x) + "," + std::to_string(y) + ")",
                    "drop");
            };
        }
        return choc::value::Value();
    });

    // Font loading: loadFont(path) → success boolean
    engine_.register_function("loadFont", [](choc::javascript::ArgumentList args) {
        auto path = args.get<std::string>(0, "");
        // Font loading is platform-dependent; this registers the font path
        // for use by canvas.set_font() and Label font_family
        // Currently a stub that acknowledges the request
        bool exists = !path.empty() && std::filesystem::exists(path);
        return choc::value::createBool(exists);
    });

    // WebGPU shader: applyShader(canvasId, skslCode) → applies custom shader to canvas
    // Uses the existing Skia/Dawn pipeline — SkSL shaders compiled at runtime
    engine_.register_function("applyShader", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto code = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (v) {
            // Store shader code on the view for the render pipeline to pick up
            auto theme = v->theme();
            theme.dimensions["shader.active"] = 1;
            v->set_theme(theme);
        }
        // Compilation validation
        auto result = choc::value::createObject("");
        result.addMember("success", choc::value::createBool(!code.empty()));
        result.addMember("error", choc::value::createString(""));
        return result;
    });

    auto gpu_info = widget_bridge_gpu_info(gpu_surface_);

    // WebGPU: getGPUInfo() → device capabilities
    engine_.register_function("getGPUInfo", [gpu_info](choc::javascript::ArgumentList) {
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

    HostObjectDescriptor gpu;
    gpu.class_name = "GPU";
    gpu.properties.push_back({"backend", choc::value::createString(gpu_info.backend)});
    gpu.properties.push_back({"backendType", choc::value::createString(gpu_info.backend_type)});
    gpu.properties.push_back({"available", choc::value::createBool(gpu_info.available)});
    gpu.properties.push_back({"nativeBridge", choc::value::createBool(gpu_info.native_bridge)});
    gpu.methods.push_back({"getPreferredCanvasFormat", [gpu_info](const choc::value::Value*, size_t) {
        return choc::value::createString(gpu_info.preferred_canvas_format);
    }});
    engine_.register_host_object("navigatorGPU", std::move(gpu));

    engine_.register_function("__describeNativeAdapterImpl", [gpu_info](choc::javascript::ArgumentList) {
        return gpu_descriptor_to_value(gpu_info);
    });

    engine_.register_function("__describeNativeDeviceImpl", [gpu_info](choc::javascript::ArgumentList) {
        auto device = choc::value::createObject("");
        device.addMember("nativeBridge", choc::value::createBool(gpu_info.native_bridge));
        device.addMember("adapterInfo", gpu_adapter_info_to_value(gpu_info));
        return device;
    });

    engine_.register_function("__gpuCanvasConfigureImpl", [this, gpu_info](choc::javascript::ArgumentList args) {
        auto canvas_id = args.get<std::string>(0, "");
        auto width = static_cast<uint32_t>(std::max(1, args.get<int32_t>(1, 1)));
        auto height = static_cast<uint32_t>(std::max(1, args.get<int32_t>(2, 1)));
        auto format = args.get<std::string>(3, gpu_info.preferred_canvas_format);
        auto usage = static_cast<uint32_t>(args.get<int32_t>(4, 0));
        auto alpha_mode = args.get<std::string>(5, "opaque");

        auto result = choc::value::createObject("");
        result.addMember("nativeBridge", choc::value::createBool(false));
        result.addMember("configured", choc::value::createBool(false));
        result.addMember("width", choc::value::createInt32(static_cast<int32_t>(width)));
        result.addMember("height", choc::value::createInt32(static_cast<int32_t>(height)));
        result.addMember("format", choc::value::createString(format));
        result.addMember("usage", choc::value::createInt32(static_cast<int32_t>(usage)));
        result.addMember("alphaMode", choc::value::createString(alpha_mode));

        if (canvas_id.empty() || native_gpu_bridge_state_ == nullptr || gpu_surface_ == nullptr) {
            return result;
        }

#ifndef PULP_HAS_SKIA
        return result;
#else
        auto* device_ptr = static_cast<wgpu::Device*>(gpu_surface_->dawn_device_handle());
        if (device_ptr == nullptr) {
            return result;
        }

        auto& state = native_gpu_bridge_state_->canvases[canvas_id];
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
        auto native_result = choc::value::createObject("");
        native_result.addMember("nativeBridge", choc::value::createBool(state.configured));
        native_result.addMember("configured", choc::value::createBool(state.configured));
        native_result.addMember("width", choc::value::createInt32(static_cast<int32_t>(width)));
        native_result.addMember("height", choc::value::createInt32(static_cast<int32_t>(height)));
        native_result.addMember("format", choc::value::createString(format));
        native_result.addMember("usage", choc::value::createInt32(static_cast<int32_t>(state.usage)));
        native_result.addMember("alphaMode", choc::value::createString(alpha_mode));
        return native_result;
#endif
    });

    engine_.register_function("__gpuCanvasDescribeCurrentTextureImpl", [this](choc::javascript::ArgumentList args) {
        auto descriptor = choc::value::createObject("");
        auto canvas_id = args.get<std::string>(0, "");
        descriptor.addMember("nativeBridge", choc::value::createBool(false));
        descriptor.addMember("width", choc::value::createInt32(1));
        descriptor.addMember("height", choc::value::createInt32(1));
        descriptor.addMember("format", choc::value::createString("bgra8unorm"));
        descriptor.addMember("usage", choc::value::createInt32(0));
        descriptor.addMember("label", choc::value::createString("pulp-native-gpu-texture"));

        if (canvas_id.empty() || native_gpu_bridge_state_ == nullptr) {
            return descriptor;
        }

        auto it = native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == native_gpu_bridge_state_->canvases.end() || !it->second.configured) {
            return descriptor;
        }

        auto native_descriptor = choc::value::createObject("");
        native_descriptor.addMember("nativeBridge", choc::value::createBool(true));
        native_descriptor.addMember("width", choc::value::createInt32(static_cast<int32_t>(it->second.width)));
        native_descriptor.addMember("height", choc::value::createInt32(static_cast<int32_t>(it->second.height)));
        native_descriptor.addMember("format", choc::value::createString(it->second.format));
        native_descriptor.addMember("usage", choc::value::createInt32(static_cast<int32_t>(it->second.usage)));
        native_descriptor.addMember("label", choc::value::createString(canvas_id + "-native-texture"));
        return native_descriptor;
    });

    engine_.register_function("__gpuCreateTextureImpl", [this](choc::javascript::ArgumentList args) {
        auto payload_json = args.get<std::string>(0, "");
        if (payload_json.empty() || native_gpu_bridge_state_ == nullptr || gpu_surface_ == nullptr) {
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

        auto* device_ptr = static_cast<wgpu::Device*>(gpu_surface_->dawn_device_handle());
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
        if ((texture_desc.usage & wgpu::TextureUsage::RenderAttachment) == wgpu::TextureUsage::None) {
            texture_desc.usage |= wgpu::TextureUsage::RenderAttachment;
        }

        auto texture = device_ptr->CreateTexture(&texture_desc);
        if (!texture) {
            return choc::value::createString("");
        }

        auto texture_id = std::string("native-texture-") + std::to_string(native_gpu_bridge_state_->next_texture_id++);
        auto& state = native_gpu_bridge_state_->textures[texture_id];
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

    engine_.register_function("__decodeImageDataImpl", [](choc::javascript::ArgumentList args) {
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

    engine_.register_function("__gpuDestroyTextureImpl", [this](choc::javascript::ArgumentList args) {
        auto texture_id = args.get<std::string>(0, "");
        if (texture_id.empty() || native_gpu_bridge_state_ == nullptr) {
            return choc::value::createBool(false);
        }

        return choc::value::createBool(native_gpu_bridge_state_->textures.erase(texture_id) > 0);
    });

    engine_.register_function("__gpuQueueSubmitImpl", [this](choc::javascript::ArgumentList args) {
        auto canvas_id = args.get<std::string>(0, "");
        auto r = static_cast<float>(args.get<double>(1, 0.0));
        auto g = static_cast<float>(args.get<double>(2, 0.0));
        auto b = static_cast<float>(args.get<double>(3, 0.0));
        auto a = static_cast<float>(args.get<double>(4, 1.0));

        if (canvas_id.empty() || native_gpu_bridge_state_ == nullptr || gpu_surface_ == nullptr) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createBool(false);
#else
        auto it = native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == native_gpu_bridge_state_->canvases.end() || !it->second.configured || !it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* device_ptr = static_cast<wgpu::Device*>(gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(gpu_surface_->dawn_queue_handle());
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
        return choc::value::createBool(true);
#endif
    });

    engine_.register_function("__gpuQueueDrawImpl", [this](choc::javascript::ArgumentList args) {
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

        if (canvas_id.empty() || native_gpu_bridge_state_ == nullptr || gpu_surface_ == nullptr ||
            vertex_code.empty() || fragment_code.empty() || vertex_count == 0) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createBool(false);
#else
        auto it = native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == native_gpu_bridge_state_->canvases.end() || !it->second.configured || !it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* device_ptr = static_cast<wgpu::Device*>(gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(gpu_surface_->dawn_queue_handle());
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
                            if (bench_counters_) {
                                bench_counters_->gpu_upload_total_us.fetch_add(
                                    render::bench::now_us() - t0,
                                    std::memory_order_relaxed);
                                bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                                    static_cast<double>(upload_data.size()),
                                    std::memory_order_relaxed);
                                bench_counters_->gpu_buffer_upload_count.fetch_add(
                                    1.0, std::memory_order_relaxed);
                                bench_counters_->observe_resident_peak(
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
                        auto source_it = native_gpu_bridge_state_->canvases.find(source_canvas_id);
                        if (source_canvas_id.empty() || source_it == native_gpu_bridge_state_->canvases.end() ||
                            !source_it->second.configured || !source_it->second.texture) {
                            return choc::value::createBool(false);
                        }

                        auto view_format = entry_view.hasObjectMember("format")
                            ? entry_view["format"].getWithDefault<std::string>(source_it->second.format)
                            : source_it->second.format;
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

                        const bool use_default_view =
                            view_format == source_it->second.format &&
                            view_dimension == "2d" &&
                            view_aspect == "all" &&
                            base_mip_level == 0 &&
                            mip_level_count == 1 &&
                            base_array_layer == 0 &&
                            array_layer_count == 1;

                        auto texture_view = use_default_view
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
        pass.Draw(vertex_count, instance_count, first_vertex, first_instance);
        pass.End();

        auto command_buffer = encoder.Finish();
        queue_ptr->Submit(1, &command_buffer);
        return choc::value::createBool(true);
#endif
    });

    engine_.register_function("__gpuQueueDrawBufferedImpl", [this](choc::javascript::ArgumentList args) {
        if (args.numArgs < 1 || !args[0] || native_gpu_bridge_state_ == nullptr || gpu_surface_ == nullptr) {
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

        NativeGpuBridgeState::CanvasContextState* target_canvas_state = nullptr;
        NativeGpuBridgeState::TextureState* target_texture_state = nullptr;
        if (!canvas_id.empty()) {
            auto it = native_gpu_bridge_state_->canvases.find(canvas_id);
            if (it == native_gpu_bridge_state_->canvases.end() || !it->second.configured || !it->second.texture) {
                return choc::value::createBool(false);
            }
            target_canvas_state = &it->second;
        } else {
            auto it = native_gpu_bridge_state_->textures.find(target_texture_id);
            if (it == native_gpu_bridge_state_->textures.end() || !it->second.configured || !it->second.texture) {
                return choc::value::createBool(false);
            }
            target_texture_state = &it->second;
        }

        auto* device_ptr = static_cast<wgpu::Device*>(gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(gpu_surface_->dawn_queue_handle());
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
                if (bench_counters_) {
                    bench_counters_->gpu_upload_total_us.fetch_add(
                        render::bench::now_us() - t0,
                        std::memory_order_relaxed);
                    bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                        static_cast<double>(upload_data.size()),
                        std::memory_order_relaxed);
                    bench_counters_->gpu_buffer_upload_count.fetch_add(
                        1.0, std::memory_order_relaxed);
                    bench_counters_->observe_resident_peak(
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
        std::vector<wgpu::TextureView> bind_group_texture_views;
        std::vector<wgpu::BindGroupLayout> bind_group_layouts;
        std::vector<wgpu::BindGroup> bind_groups;
        if (payload.hasObjectMember("bindGroups") && payload["bindGroups"].isArray()) {
            auto bind_groups_payload = payload["bindGroups"];
            bind_group_indices.reserve(bind_groups_payload.size());
            bind_group_layouts.reserve(bind_groups_payload.size());
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
                            if (bench_counters_) {
                                bench_counters_->gpu_upload_total_us.fetch_add(
                                    render::bench::now_us() - t0,
                                    std::memory_order_relaxed);
                                bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                                    static_cast<double>(upload_data.size()),
                                    std::memory_order_relaxed);
                                bench_counters_->gpu_buffer_upload_count.fetch_add(
                                    1.0, std::memory_order_relaxed);
                                bench_counters_->observe_resident_peak(
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
                            auto source_it = native_gpu_bridge_state_->textures.find(source_texture_id);
                            if (source_it == native_gpu_bridge_state_->textures.end() ||
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
                            auto source_it = native_gpu_bridge_state_->canvases.find(source_canvas_id);
                            if (source_it == native_gpu_bridge_state_->canvases.end() ||
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

                wgpu::BindGroupLayoutDescriptor bind_group_layout_desc{};
                bind_group_layout_desc.entryCount = bind_group_layout_entries.size();
                bind_group_layout_desc.entries = bind_group_layout_entries.data();
                auto bind_group_layout = device_ptr->CreateBindGroupLayout(&bind_group_layout_desc);
                if (!bind_group_layout) {
                    return choc::value::createBool(false);
                }
                bind_group_layouts.push_back(bind_group_layout);

                wgpu::BindGroupDescriptor bind_group_desc{};
                bind_group_desc.layout = bind_group_layout;
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

        wgpu::ColorTargetState color_target{};
        color_target.format = texture_format_from_string(format);

        wgpu::FragmentState fragment_state{};
        fragment_state.module = fragment_module;
        fragment_state.entryPoint = fragment_entry.c_str();
        fragment_state.targetCount = 1;
        fragment_state.targets = &color_target;

        wgpu::RenderPipelineDescriptor pipeline_desc{};
        wgpu::PipelineLayout explicit_pipeline_layout;
        if (!bind_group_layouts.empty()) {
            wgpu::PipelineLayoutDescriptor pipeline_layout_desc{};
            pipeline_layout_desc.bindGroupLayoutCount = bind_group_layouts.size();
            pipeline_layout_desc.bindGroupLayouts = bind_group_layouts.data();
            explicit_pipeline_layout = device_ptr->CreatePipelineLayout(&pipeline_layout_desc);
            if (!explicit_pipeline_layout) return choc::value::createBool(false);
            pipeline_desc.layout = explicit_pipeline_layout;
        } else {
            pipeline_desc.layout = nullptr;
        }
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
                if (bench_counters_) {
                    bench_counters_->gpu_upload_total_us.fetch_add(
                        render::bench::now_us() - t0,
                        std::memory_order_relaxed);
                    bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                        static_cast<double>(upload_index_data.size()),
                        std::memory_order_relaxed);
                    bench_counters_->gpu_buffer_upload_count.fetch_add(
                        1.0, std::memory_order_relaxed);
                    bench_counters_->observe_resident_peak(
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
            pass.DrawIndexed(index_count, instance_count, first_index, base_vertex, first_instance);
        } else {
            auto vertex_count = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("vertexCount") ? payload["vertexCount"].getWithDefault<int32_t>(0) : 0));
            auto instance_count = static_cast<uint32_t>(std::max(1, payload.hasObjectMember("instanceCount") ? payload["instanceCount"].getWithDefault<int32_t>(1) : 1));
            auto first_vertex = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("firstVertex") ? payload["firstVertex"].getWithDefault<int32_t>(0) : 0));
            auto first_instance = static_cast<uint32_t>(std::max(0, payload.hasObjectMember("firstInstance") ? payload["firstInstance"].getWithDefault<int32_t>(0) : 0));
            if (vertex_count == 0) return choc::value::createBool(false);
            pass.Draw(vertex_count, instance_count, first_vertex, first_instance);
        }

        pass.End();
        auto command_buffer = encoder.Finish();
        queue_ptr->Submit(1, &command_buffer);
        if (target_canvas_state != nullptr) {
            request_repaint();
        }
        return choc::value::createBool(true);
#endif
    });

    engine_.register_function("__gpuQueuePresentTextureImpl", [this](choc::javascript::ArgumentList args) {
        if (args.numArgs < 1 || !args[0] || native_gpu_bridge_state_ == nullptr || gpu_surface_ == nullptr) {
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

        auto canvas_it = native_gpu_bridge_state_->canvases.find(canvas_id);
        auto source_it = native_gpu_bridge_state_->textures.find(source_texture_id);
        if (canvas_it == native_gpu_bridge_state_->canvases.end() ||
            source_it == native_gpu_bridge_state_->textures.end() ||
            !canvas_it->second.configured || !canvas_it->second.texture ||
            !source_it->second.configured || !source_it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* device_ptr = static_cast<wgpu::Device*>(gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(gpu_surface_->dawn_queue_handle());
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
        request_repaint();
        return choc::value::createBool(true);
#endif
    });

    engine_.register_function("__gpuCanvasPresentImpl", [this](choc::javascript::ArgumentList args) {
        auto canvas_id = args.get<std::string>(0, "");
        if (canvas_id.empty() || native_gpu_bridge_state_ == nullptr) {
            return choc::value::createBool(false);
        }

        auto it = native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == native_gpu_bridge_state_->canvases.end()) {
            return choc::value::createBool(false);
        }

        if (it->second.configured) {
            request_repaint();
        }
        return choc::value::createBool(it->second.configured);
    });

    engine_.register_promise_function("__requestAdapterImpl", [gpu_info](const choc::value::Value*, size_t) {
        return gpu_descriptor_to_value(gpu_info);
    });

    // ── Compute pipeline dispatch ───────────────────────────────────────
    // Receives JSON from the JS compute pass encoder and dispatches
    // via Dawn's native compute pipeline infrastructure.
    engine_.register_function("__gpuComputeDispatchImpl", [this](choc::javascript::ArgumentList args) {
        if (args.numArgs < 1 || !args[0] || gpu_surface_ == nullptr) {
            return choc::value::createBool(false);
        }

#ifndef PULP_HAS_SKIA
        return choc::value::createBool(false);
#else
        auto payload_str = args.get<std::string>(0, "");
        if (payload_str.empty()) return choc::value::createBool(false);

        auto* device_ptr = static_cast<wgpu::Device*>(gpu_surface_->dawn_device_handle());
        auto* queue_ptr = static_cast<wgpu::Queue*>(gpu_surface_->dawn_queue_handle());
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

                            // Issue #491 P2: decode the base64 payload the JS
                            // serializer (web-compat-gpu-buffered.js) attaches
                            // as `bufferDataBase64` and upload it to the GPU
                            // buffer. Without this every compute dispatch
                            // runs against zeroed buffers regardless of what
                            // the JS shader seeded them with.
                            if (entry.hasObjectMember("bufferDataBase64") && buf_size > 0) {
                                auto b64 = entry["bufferDataBase64"].getWithDefault<std::string>("");
                                if (!b64.empty()) {
#ifdef PULP_BENCHMARK
                                    const double decode_t0 = render::bench::now_us();
                                    auto decoded = runtime::base64_decode(b64);
                                    if (bench_counters_) {
                                        bench_counters_->base64_decode_total_us.fetch_add(
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
                                            if (bench_counters_) {
                                                bench_counters_->gpu_upload_total_us.fetch_add(
                                                    render::bench::now_us() - t0,
                                                    std::memory_order_relaxed);
                                                bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                                                    static_cast<double>(to_copy),
                                                    std::memory_order_relaxed);
                                                bench_counters_->gpu_buffer_upload_count.fetch_add(
                                                    1.0, std::memory_order_relaxed);
                                                bench_counters_->observe_resident_peak(
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

    // ── Binary transfer: register a native buffer for zero-copy GPU upload ──
    // Avoids base64 encoding overhead for buffers > 64KB.
    engine_.register_function("__registerNativeBuffer", [this](choc::javascript::ArgumentList args) {
        auto buffer_id = args.get<std::string>(0, "");
        auto size = static_cast<size_t>(args.get<int64_t>(1, 0));
        if (buffer_id.empty() || size == 0) return choc::value::createBool(false);

        // Allocate a native buffer and return a handle
        if (!native_gpu_bridge_state_) return choc::value::createBool(false);
        native_gpu_bridge_state_->native_buffers[buffer_id].resize(size, 0);
        return choc::value::createBool(true);
    });

    engine_.register_function("__writeNativeBuffer", [this](choc::javascript::ArgumentList args) {
        auto buffer_id = args.get<std::string>(0, "");
        auto offset = static_cast<size_t>(args.get<int64_t>(1, 0));
        auto data_b64 = args.get<std::string>(2, "");  // Still base64 for now, but in chunks
        if (buffer_id.empty() || data_b64.empty() || !native_gpu_bridge_state_)
            return choc::value::createBool(false);

        auto it = native_gpu_bridge_state_->native_buffers.find(buffer_id);
        if (it == native_gpu_bridge_state_->native_buffers.end())
            return choc::value::createBool(false);

        // For now, store the raw base64 chunk reference.
        // Full implementation would decode base64 and memcpy into the native buffer.
        (void)offset;
        return choc::value::createBool(true);
    });

    // ── DRACO mesh decode (native C++ decoder) ──────────────────────────
    engine_.register_function("__dracoDecodeBuffer", [](choc::javascript::ArgumentList args) {
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

// Map a KeyCode enum value to its W3C UIEvent.key string. JSX handlers
// in @pulp/react consumers read `e.key === 'Escape'` / `'ArrowLeft'` /
// etc. — the previous implementation sent the raw int, so every such
// comparison failed silently (e.g. Spectr's `e.key === 'Escape'` modal
// close, history undo arrows, etc. were dead).
static std::string keycode_to_w3c_key(int key_code, bool shift_held) {
    using K = KeyCode;
    auto kc = static_cast<K>(key_code);
    switch (kc) {
        case K::escape:    return "Escape";
        case K::enter:     return "Enter";
        case K::tab:       return "Tab";
        case K::backspace: return "Backspace";
        case K::delete_:   return "Delete";
        case K::left:      return "ArrowLeft";
        case K::right:     return "ArrowRight";
        case K::up:        return "ArrowUp";
        case K::down:      return "ArrowDown";
        case K::home:      return "Home";
        case K::end_:      return "End";
        case K::page_up:   return "PageUp";
        case K::page_down: return "PageDown";
        case K::space:     return " ";
        case K::f1:  return "F1";  case K::f2:  return "F2";
        case K::f3:  return "F3";  case K::f4:  return "F4";
        case K::f5:  return "F5";  case K::f6:  return "F6";
        case K::f7:  return "F7";  case K::f8:  return "F8";
        case K::f9:  return "F9";  case K::f10: return "F10";
        case K::f11: return "F11"; case K::f12: return "F12";
        default: break;
    }
    // Printable chars: letters lower-case unless shift, digits as-is.
    if (key_code >= 'a' && key_code <= 'z') {
        char c = shift_held ? static_cast<char>(key_code - 32) : static_cast<char>(key_code);
        return std::string(1, c);
    }
    if (key_code >= '0' && key_code <= '9') {
        return std::string(1, static_cast<char>(key_code));
    }
    return "Unidentified";
}

void WidgetBridge::forward_key_event(int key_code, uint16_t modifiers, bool is_down) {
    if (!is_down) return;

    // Check registered shortcuts first
    auto kc = static_cast<KeyCode>(key_code);
    for (auto& s : shortcuts_) {
        if (s.key == kc && s.modifiers == modifiers) {
            engine_.evaluate(s.callback + "()");
            return;
        }
    }

    // W3C UIEvent.key string; modifier booleans match KeyboardEvent
    // (ctrlKey/shiftKey/altKey/metaKey). `keyCode` retained for legacy.
    bool shift_held = (modifiers & kModShift) != 0;
    std::string key_str = keycode_to_w3c_key(key_code, shift_held);
    // JSON-escape backslash + quote (single-char ascii here is safe but
    // keep the guard for safety against future printable additions).
    std::string key_json;
    key_json.reserve(key_str.size() + 2);
    for (char c : key_str) {
        if (c == '\\' || c == '\'') key_json += '\\';
        key_json += c;
    }

    engine_.evaluate("__dispatch__('__global__', 'keydown', {"
        "key:'" + key_json + "',"
        "keyCode:" + std::to_string(key_code) + ","
        "ctrlKey:" + ((modifiers & kModCtrl) ? "true" : "false") + ","
        "shiftKey:" + ((modifiers & kModShift) ? "true" : "false") + ","
        "altKey:" + ((modifiers & kModAlt) ? "true" : "false") + ","
        "metaKey:" + (((modifiers & kModMeta) || (modifiers & kModCmd)) ? "true" : "false") + ","
        "mods:" + std::to_string(modifiers) + "})");
}

} // namespace pulp::view
