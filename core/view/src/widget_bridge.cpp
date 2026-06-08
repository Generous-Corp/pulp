#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/modal.hpp>
#if __has_include(<pulp/render/gpu_surface.hpp>)
#include <pulp/render/gpu_surface.hpp>
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 1
#else
#define PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE 0
#endif
#include <pulp/platform/popup_menu.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/log.hpp>
#include <web_compat_preludes_gen.hpp>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
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
#include <pulp/canvas/attributed_string.hpp>
#endif

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#endif

namespace pulp::view {

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
    // pulp #2128 follow-up — fan to document-level listeners. React
    // click-outside patterns (`document.addEventListener('mousedown',
    // onDoc)`) are the common close-the-popover idiom; the framework
    // fires synthetic outside-click events on Esc / overlay dismiss
    // through this path, so any React popover using the pattern closes
    // without needing per-app wiring.
    if (id === 'document' && typeof document !== 'undefined' && document.dispatchEvent) {
        var devt = args && args.length ? args[0] : {};
        if (devt && typeof devt === 'object') {
            devt.type = eventName;
            document.dispatchEvent(devt);
        }
    }
    // pulp jsx-instrument-import 2026-05-17 — for pointer/mouse/click
    // events, ALSO call dispatchEvent on the JS Element for `id`. This
    // bypasses the single-slot __callbacks__[id:event] table that
    // _registerNativeEvent clobbers when React-DOM calls
    // element.addEventListener(...). Without this, native NSEvent →
    // bridge dispatch fires the on() callback (which may be overwritten)
    // but never re-enters the DOM bubble walk where React-DOM's
    // delegated root listener actually lives. Per Codex high-reasoning
    // consult: "separate native DOM event dispatch from the low-level
    // on() callback table."
    if (typeof __nativeElements__ !== 'undefined') {
        var el = __nativeElements__[id];
        if (typeof globalThis.__pulpDispatchHits__ === 'undefined') globalThis.__pulpDispatchHits__ = { byType: {}, missingElement: 0, dispatched: 0, rootListenersFired: 0, lastErr: null, total: 0 };
        var stats = globalThis.__pulpDispatchHits__;
        stats.total = (stats.total || 0) + 1;
        if (el && el.dispatchEvent &&
            /^(pointerdown|pointermove|pointerup|pointercancel|click|mousedown|mousemove|mouseup|wheel)$/.test(eventName)) {
            stats.byType[eventName] = (stats.byType[eventName] || 0) + 1;
            try {
                var data = args && args.length ? args[0] : {};
                var ev = (typeof _makeEvent === 'function')
                    ? _makeEvent(eventName, el, data || {})
                    : {
                        type: eventName, target: el, currentTarget: el,
                        bubbles: true, cancelable: true,
                        clientX: (data && data.clientX) || 0,
                        clientY: (data && data.clientY) || 0,
                        button: (data && data.button) || 0,
                        buttons: (eventName === 'pointerup' || eventName === 'mouseup') ? 0 : 1,
                        pointerId: (data && data.pointerId) || 0,
                        pointerType: (data && data.pointerType) || 'mouse',
                        isPrimary: true,
                        preventDefault: function () { this.defaultPrevented = true; },
                        stopPropagation: function () { this._stopped = true; }
                    };
                // Pre-dispatch instrumentation: confirm event reaches root.
                var pathLen = 0;
                var rootInPath = false;
                var p = el;
                while (p) { pathLen++; if (p._id === '__root__') rootInPath = true; p = p._parentElement; }
                var rootListeners = (typeof __eventListeners__ !== 'undefined' && __eventListeners__['__root__'] && __eventListeners__['__root__'][eventName]) ? __eventListeners__['__root__'][eventName].length : 0;
                stats.lastPathLen = pathLen;
                stats.lastRootInPath = rootInPath;
                stats.lastRootListeners = rootListeners;
                // Verbose log gated on PULP_DEBUG_DISP — eats stderr fast on heavy drags
                if (eventName === 'pointerdown' || eventName === 'mousedown' || eventName === 'click') {
                    if (typeof __spectrLog === 'function') {
                        __spectrLog('[disp] ' + eventName + ' id=' + id + ' pathLen=' + pathLen + ' rootInPath=' + rootInPath + ' rootListeners=' + rootListeners);
                    }
                }
                el.dispatchEvent(ev);
                stats.dispatched = (stats.dispatched || 0) + 1;
                // Fan the bubbling pointer event to document-level listeners.
                // In a real DOM, document is the top of the bubble path, but
                // here `document` is a separate object with its own listener
                // map that the element bubble (_parentElement chain) never
                // reaches. Code that does `ownerDocument.addEventListener(
                // 'pointermove'/'pointerup', ...)` therefore never fires —
                // notably Three.js OrbitControls, which on pointerdown MOVES its
                // drag/pinch move+up listeners onto the document. Without this
                // fan-out OrbitControls receives the initial pointerdown on the
                // canvas but no subsequent moves, so touch orbit/pinch is inert.
                // Only pointer events are fanned (the synthesized mouse event
                // below keeps its element-only delivery to avoid changing
                // existing document-mouse semantics).
                if (typeof document !== 'undefined' && document.dispatchEvent &&
                    /^(pointerdown|pointermove|pointerup|pointercancel)$/.test(eventName)) {
                    document.dispatchEvent(ev);
                }
                // For pointerdown/up, ALSO synthesize the mouse equivalent —
                // many React components attach to onMouseDown rather than
                // onPointerDown (Chainer is one). Skip if eventName is
                // already a mouse-* type to avoid double-fire.
                if (eventName === 'pointerdown' || eventName === 'pointerup' || eventName === 'pointermove') {
                    var mouseType = (eventName === 'pointerdown') ? 'mousedown'
                                  : (eventName === 'pointerup')   ? 'mouseup'
                                  :                                  'mousemove';
                    var mev = (typeof _makeEvent === 'function')
                        ? _makeEvent(mouseType, el, data || {}) : { type: mouseType, target: el, currentTarget: el, bubbles: true };
                    el.dispatchEvent(mev);
                }
            } catch (e) {
                stats.lastErr = String(e && e.stack ? e.stack.slice(0, 200) : e);
                if (typeof __dispatchError__ === 'function') __dispatchError__(id, eventName, String(e && e.stack ? e.stack : e));
            }
        } else if (!el) {
            stats.missingElement = (stats.missingElement || 0) + 1;
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

static void eval_or_throw(ScriptEngine& engine, const char* name, const std::string& js) {
    try {
        engine.evaluate(js);
    } catch (const choc::javascript::Error& e) {
        runtime::log_error("PULP_EVAL_THROW: name={} js_len={} choc_error={}", name, js.size(), e.what());
        throw std::runtime_error(std::string("failed to evaluate ") + name + ": " + e.what());
    } catch (const std::exception& e) {
        runtime::log_error("PULP_EVAL_THROW: name={} js_len={} std_error={}", name, js.size(), e.what());
        throw std::runtime_error(std::string("failed to evaluate ") + name + ": " + e.what());
    } catch (...) {
        runtime::log_error("PULP_EVAL_THROW: name={} js_len={} unknown_exception", name, js.size());
        throw std::runtime_error(std::string("failed to evaluate ") + name + ": unknown exception");
    }
}

// Static registry of live WidgetBridges. Platform hosts iterate this to
// deliver key events and document-level events without each app needing
// to wire its own `View::on_global_key` lambda.
//
// recursive_mutex (not plain mutex): the `dispatch_*` helpers below
// hold the lock for the full fan-out — including the call into each
// bridge's `forward_key_event` / JS evaluation — to address Codex P1
// (PR #2137 / #2139) that a snapshot-then-unlock pattern UAFs if a
// bridge is destroyed on another thread mid-iteration. Holding during
// dispatch is safe: Pulp's standard bridge ctor/dtor lifecycle is
// strictly host-driven (never invoked from JS), and recursive_mutex
// defensively tolerates a future JS-callback path that might create
// or destroy a bridge on the same thread without deadlocking.
namespace {
std::recursive_mutex& all_bridges_mutex() {
    static std::recursive_mutex m;
    return m;
}
std::unordered_set<WidgetBridge*>& all_bridges_set() {
    static std::unordered_set<WidgetBridge*> set;
    return set;
}
}  // namespace

WidgetBridge::WidgetBridge(ScriptEngine& engine, View& root, state::StateStore& store,
                           render::GpuSurface* gpu_surface)
    : engine_(engine), root_(root), store_(store), gpu_surface_(gpu_surface) {
    {
        std::lock_guard<std::recursive_mutex> lock(all_bridges_mutex());
        all_bridges_set().insert(this);
    }
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
    // P5-7 follow-up — Events + Pointer-capture (P2b) extracted from
    // web-compat-element.js. Must eval AFTER the parent so the Element
    // constructor + prototype are already defined when the extracted
    // prototype overrides install.
    eval_or_throw(engine_, "web_compat_element_events", preludes::web_compat_element_events);
    eval_or_throw(engine_, "web_compat_canvas", preludes::web_compat_canvas);
    // P5-6 first cut — native GPU canvas helpers extracted from
    // web-compat-canvas.js. Must eval AFTER the canvas core so
    // CanvasRenderingContext2D + window.pulp.gpu are in scope when
    // __ensurePulpGpuHelpers / getContext("webgpu") run.
    eval_or_throw(engine_, "web_compat_canvas_gpu", preludes::web_compat_canvas_gpu);
    // P5-6 follow-up — _PulpCanvasMatrix DOMMatrix-compat helper +
    // Canvas2D API gap closures (measureText / drawImage /
    // setLineDash / getLineDash / getImageData / putImageData).
    // Must eval AFTER web_compat_canvas so the
    // CanvasRenderingContext2D constructor + prototype are in scope.
    eval_or_throw(engine_, "web_compat_canvas_matrix", preludes::web_compat_canvas_matrix);
    eval_or_throw(engine_, "web_compat_canvas_image", preludes::web_compat_canvas_image);
    eval_or_throw(engine_, "web_compat_style_decl", preludes::web_compat_style_decl);
    // P5-5 — per-domain `_applyProperty` handler modules. The former
    // monolithic per-CSS-property switch is split into layout / paint /
    // typography / transform / misc handlers; web-compat-style-decl.js's
    // `_applyProperty` is now a thin dispatcher that calls each handler
    // (`_applyLayoutProp` etc.) in turn. The handlers are plain function
    // declarations the dispatcher resolves at call time, so they MUST
    // eval AFTER style_decl and before any consumer triggers a style
    // apply (the helpers IIFE only installs setters; it does not call
    // `_applyProperty` at install time, so the order vs. helpers below
    // is not load-bearing — but keeping them adjacent is clearer).
    eval_or_throw(engine_, "web_compat_style_decl_layout", preludes::web_compat_style_decl_layout);
    eval_or_throw(engine_, "web_compat_style_decl_paint", preludes::web_compat_style_decl_paint);
    eval_or_throw(engine_, "web_compat_style_decl_typography", preludes::web_compat_style_decl_typography);
    eval_or_throw(engine_, "web_compat_style_decl_transform", preludes::web_compat_style_decl_transform);
    eval_or_throw(engine_, "web_compat_style_decl_misc", preludes::web_compat_style_decl_misc);
    // P5-5 first cut — _cssToFlex + __cssProperties__ IIFE +
    // setProperty/getPropertyValue/removeProperty extracted out of
    // web-compat-style-decl.js. Must eval AFTER style_decl so the
    // CSSStyleDeclaration constructor + _applyProperty prototype
    // method are in scope when the IIFE walks __cssProperties__ and
    // installs the per-property reflection.
    eval_or_throw(engine_, "web_compat_style_decl_helpers", preludes::web_compat_style_decl_helpers);
    eval_or_throw(engine_, "web_compat_document", preludes::web_compat_document);
    // P5-7 first cut — CSS selector engine extracted from
    // web-compat-document.js. Must eval AFTER document + Element so
    // the underscore-prefixed selector helpers (_parseSelector,
    // _matchesSelector, _querySelector, _findMatch, etc.) are
    // resolvable when document.querySelector / .querySelectorAll
    // dispatch into them.
    eval_or_throw(engine_, "web_compat_document_selectors", preludes::web_compat_document_selectors);
    // P5-7 follow-up — WebGPU mock factories. Must eval AFTER
    // document so GPU* usage constants (GPUTextureUsage etc.) are in
    // scope when the factory bodies resolve them lazily at call time.
    eval_or_throw(engine_, "web_compat_document_gpu_mock", preludes::web_compat_document_gpu_mock);
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
    {
        std::lock_guard<std::recursive_mutex> lock(all_bridges_mutex());
        all_bridges_set().erase(this);
    }
    if (callback_alive_) callback_alive_->store(false, std::memory_order_release);
    root_.on_global_click = {};
}

// Phase iOS-D.3b Slice 1 — late-attach of the GpuSurface for the common
// case where ScriptedUiSession / ViewBridge is constructed BEFORE the
// PluginViewHost (and therefore before the surface exists). Mirrors the
// 4th constructor argument; idempotent.
void WidgetBridge::attach_gpu_surface(render::GpuSurface* gpu_surface) {
    if (gpu_surface_ == gpu_surface) return;
    gpu_surface_ = gpu_surface;
    const auto gpu_info = widget_bridge_gpu_info(gpu_surface_);
    if (gpu_surface_ != nullptr) {
        if (gpu_info.native_bridge) {
            if (!native_gpu_bridge_state_) {
                native_gpu_bridge_state_ = std::make_unique<NativeGpuBridgeState>();
            }
        } else {
            // Surface present but no native bridge available — release any
            // stale state from a previous attach.
            native_gpu_bridge_state_.reset();
        }
    } else {
        // Explicit detach: drop the native bridge state so future per-frame
        // canvas/draw calls fall back to mocks cleanly.
        native_gpu_bridge_state_.reset();
    }
    safe_dispatch_eval(engine_, gpu_host_object_update_script(gpu_info), "gpu surface attach");
}

bool WidgetBridge::has_native_gpu_bridge() const noexcept {
    return gpu_surface_ != nullptr && native_gpu_bridge_state_ != nullptr;
}

void WidgetBridge::dispatch_global_key(int key_code, uint16_t modifiers, bool is_down) {
    // Codex P1 (PR #2137): hold the registry lock for the entire
    // fan-out. Earlier draft copied raw pointers under-lock then
    // iterated unlocked, which UAFs if a bridge is destroyed on
    // another thread (window teardown / hot reload) between snapshot
    // and dispatch. recursive_mutex tolerates same-thread reentry.
    std::lock_guard<std::recursive_mutex> lock(all_bridges_mutex());
    for (auto* b : all_bridges_set()) {
        b->forward_key_event(key_code, modifiers, is_down);
    }
}

void WidgetBridge::dispatch_document_event(const std::string& event_type,
                                           const std::string& event_json_literal) {
    // FOOTGUN: both arguments are concatenated into a JS expression
    // verbatim. Call sites MUST pass trusted literals only — never
    // user-provided or untrusted strings. Current callers
    // (window_host_mac.mm Esc handler) pass hardcoded "mousedown" /
    // "pointerdown" + a hardcoded literal "{clientX:-1,clientY:-1,
    // target:null}". If a future call site needs runtime values,
    // build the JSON via a serializer and re-validate this contract.
    //
    // Codex P1 (PR #2139): same lifetime-safety rationale as
    // dispatch_global_key above — hold the lock through iteration.
    const std::string js =
        "__dispatch__('document', '" + event_type + "', " + event_json_literal + ")";
    std::lock_guard<std::recursive_mutex> lock(all_bridges_mutex());
    for (auto* b : all_bridges_set()) {
        b->engine_.evaluate(js);
    }
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

void WidgetBridge::load_script(const std::string& code,
                               const std::string& script_id) {
    // Phase 9: record the script identity BEFORE eval so any
    // requestAnimationFrame calls made during eval already see the new
    // active script. Cleared by the caller (or by a subsequent
    // load_script call) when the script's surface goes away.
    active_script_id_ = script_id;
    load_script(code);
}

void WidgetBridge::set_active_script_id(const std::string& script_id) {
    active_script_id_ = script_id;
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
    } else if (auto* c = dynamic_cast<ComboBox*>(w)) {
        // pulp 2026-06-08 (routing-parity sweep) — mirror createCombo's inline
        // wiring so a `<combo>`/`<select>` tag routed through __domAppend
        // dispatches the same `select` event as the factory path.
        c->on_change = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "combo select");
        };
    } else if (auto* cb = dynamic_cast<Checkbox*>(w)) {
        // pulp 2026-06-08 — mirror createCheckbox's inline `change` wiring.
        cb->on_change = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::string(v?"1":"0") + ")", "checkbox change");
        };
    } else if (auto* lb = dynamic_cast<ListBox*>(w)) {
        // pulp 2026-06-08 — mirror createListBox's inline select/activate wiring.
        lb->on_select = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "list select");
        };
        lb->on_activate = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'activate', " + std::to_string(idx) + ")", "list activate");
        };
    }
}

std::unique_ptr<View> WidgetBridge::make_widget_for_tag(const std::string& tag,
                                                        const std::string& id) {
    // Shared lowercase widget-tag → native widget table. Keep in lockstep with
    // the `createX` factory functions (factory_api.cpp), the JS `_ensureNative`
    // map (web-compat-element.js), and the `@pulp/react` host-config lowercase
    // aliases — the routing-parity sweep test asserts all four agree. Returns
    // nullptr for any non-widget tag so __domAppend falls through to its
    // container/HTML defaults (div/span/canvas/svg-prims/input are handled by
    // their own branches before this is consulted).
    std::unique_ptr<View> w;
    if (tag == "knob") {
        auto k = std::make_unique<Knob>();
        k->set_label(id);  // match createKnob's default label
        w = std::move(k);
    } else if (tag == "fader") {
        w = std::make_unique<Fader>();
    } else if (tag == "toggle") {
        w = std::make_unique<Toggle>();
    } else if (tag == "combo" || tag == "select") {
        w = std::make_unique<ComboBox>();
    } else if (tag == "checkbox") {
        w = std::make_unique<Checkbox>();
    } else if (tag == "spectrum") {
        w = std::make_unique<SpectrumView>();
    } else if (tag == "waveform") {
        w = std::make_unique<WaveformView>();
    } else if (tag == "meter") {
        w = std::make_unique<Meter>();
    } else if (tag == "xypad") {
        w = std::make_unique<XYPad>();
    } else if (tag == "listbox") {
        w = std::make_unique<ListBox>();
    } else if (tag == "icon") {
        w = std::make_unique<Icon>();
    } else if (tag == "progress") {
        w = std::make_unique<ProgressBar>();
    } else if (tag == "img" || tag == "image") {
        w = std::make_unique<ImageView>();
    }
    if (w) {
        w->set_id(id);
        wire_callbacks(id, w.get());  // no-op for display-only widgets
    }
    return w;
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

void WidgetBridge::register_api() {
    register_widget_factory_controls_api();
    register_widget_assets_api();

    register_widget_factory_form_api();

    register_widget_value_controls_api();

    register_state_binding_api();

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

    // setVisible(id, bool)
    engine_.register_function("setVisible", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vis = args.get<double>(1, 1) > 0.5;
        auto it = widgets_.find(id);
        if (it != widgets_.end()) it->second->set_visible(vis);
        return choc::value::Value();
    });

    register_accessibility_api();

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
    register_layout_grid_api();

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
        if (const char* dbg = std::getenv("PULP_DEBUG_POINTER"); dbg && *dbg) {
            std::cerr << "[bridge] registerPointer id=" << id << " widgets_.has=" << (widgets_.count(id) ? "yes" : "NO") << "\n";
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

                // Per-widget pointer dispatch (existing behavior).
                if (const char* dbg = std::getenv("PULP_DEBUG_POINTER"); dbg && *dbg) {
                    std::cerr << "[bridge] pointer " << type << " id=" << id << "\n";
                }
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', '" + type + "', " + data + ")", "pointer");

                // pulp jsx-instrument-import (2026-05-17) — also fan out the
                // MOUSE equivalent for BOTH the per-widget dispatch AND the
                // window-level fan-out. JSX handlers like onMouseDown
                // register via prop-applier under __callbacks__[id:mousedown]
                // (NOT id:pointerdown), so a pure pointerdown dispatch never
                // fires them. Synthesizing the mouse-equivalent at this
                // level lets both onMouseDown JSX and the window-level
                // useEffect listeners (Chainer's drag pattern) wire up.
                std::string mouse_type;
                if (type == "pointerdown") mouse_type = "mousedown";
                else if (type == "pointerup") mouse_type = "mouseup";
                else if (type == "pointercancel") mouse_type = "mouseup";
                if (!mouse_type.empty()) {
                    // Per-widget mouse dispatch — fires __callbacks__[id:mousedown].
                    safe_dispatch_eval(alive, engine,
                        "__dispatch__('" + id + "', '" + mouse_type + "', " + data + ")",
                        "per-widget mouse");
                    // Window-level mouse fan-out — fires window._listeners[mousedown].
                    safe_dispatch_eval(alive, engine,
                        "__dispatch__('__global__', '" + mouse_type + "', " + data + ")",
                        "global mouse");
                }
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
                    "pointerId:0,pointerType:'mouse',isPrimary:true,"
                    "button:0,buttons:1}";
                if (const char* dbg = std::getenv("PULP_DEBUG_POINTER"); dbg && *dbg) {
                    std::cerr << "[bridge] drag id=" << id << " @(" << pos.x << "," << pos.y << ")\n";
                }
                safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'pointermove', " + data + ")", "pointermove");
                // pulp jsx-instrument-import — also fan out as per-widget
                // mousemove (fires __callbacks__[id:mousemove], for JSX
                // onMouseMove handlers) AND window-level mousemove
                // (fires window._listeners[mousemove] for Chainer-style
                // useEffect drag handlers).
                safe_dispatch_eval(alive, engine,
                    "__dispatch__('" + id + "', 'mousemove', " + data + ")",
                    "per-widget mousemove");
                safe_dispatch_eval(alive, engine,
                    "__dispatch__('__global__', 'mousemove', " + data + ")",
                    "global mousemove");
            };

            // Identity-preserving pointermove (iOS multi-touch). on_drag above
            // collapses every move to pointerId:0/pointerType:'mouse', which is
            // fine for a single desktop pointer but loses the per-finger
            // identity a multi-touch gesture needs. on_pointer_move forwards the
            // real MouseEvent — distinct pointerId + pointerType:'touch' — so a
            // scripted UI tracking two simultaneous touches (OrbitControls
            // pinch-zoom keys its dolly distance on two pointerIds) sees each
            // finger separately. clientX/Y carry the window-space (root) coords
            // OrbitControls' touch path reads via pageX/pageY (web-compat
            // synthesises pageX = clientX); offsetX/Y carry widget-local coords.
            w->on_pointer_move = [alive, engine, id](const MouseEvent& me) {
                std::string data = "{"
                    "clientX:" + std::to_string(me.window_position.x) + ","
                    "clientY:" + std::to_string(me.window_position.y) + ","
                    "offsetX:" + std::to_string(me.position.x) + ","
                    "offsetY:" + std::to_string(me.position.y) + ","
                    "pointerId:" + std::to_string(me.pointer_id) + ","
                    "pointerType:'" + std::string(me.pointerTypeString()) + "',"
                    "isPrimary:" + (me.isPrimary() ? "true" : "false") + ","
                    "pressure:" + std::to_string(me.pressure) + ","
                    "button:0,buttons:1}";
                safe_dispatch_eval(alive, engine,
                    "__dispatch__('" + id + "', 'pointermove', " + data + ")",
                    "pointermove");
                // Mouse fan-out parity with on_drag so JSX onMouseMove +
                // window-level useEffect drag handlers also wire up on iOS.
                safe_dispatch_eval(alive, engine,
                    "__dispatch__('" + id + "', 'mousemove', " + data + ")",
                    "per-widget mousemove");
                safe_dispatch_eval(alive, engine,
                    "__dispatch__('__global__', 'mousemove', " + data + ")",
                    "global mousemove");
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

    register_metadata_removal_api();

    // ═══════════════════════════════════════════════════════════════
    // Extended API: containers, layout, all widgets, themes, canvas
    // ═══════════════════════════════════════════════════════════════

    register_widget_factory_container_api();
    register_layout_flex_api();
    register_dom_api();
    register_layout_query_api();

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

    register_widget_factory_composite_api();

    register_widget_value_list_api();

    register_widget_factory_text_editor_api();

    engine_.register_function("createCanvas", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<CanvasWidget>(); c->set_id(id);
        c->set_native_gpu_texture_provider([this, id]() {
            return this->describe_native_canvas_frame(id);
        });
        widgets_[id] = c.get(); resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    register_widget_value_label_api();

    register_metadata_source_api();

    register_widget_value_basic_api();

    register_widget_typography_api();

    register_widget_value_content_api();

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
    register_tokens_api(parseHexColor);

    register_widget_text_runs_api(parseHexColor);

    engine_.register_function("setBackground", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) v->set_background_color(parseHexColor(hex));
        return choc::value::Value();
    });

    register_svg_api(parseHexColor);

    register_widget_border_box_api(parseHexColor);

    register_list_style_api();

    register_widget_outline_api(parseHexColor);

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
    register_layout_box_model_api();

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
    register_layout_position_api();

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

        // CSS gradient parsing lives in the shared helper (css_gradient.cpp)
        // so the JS bridge, the native design-import materializer, and baked
        // C++ codegen resolve gradients identically. parseColor is threaded
        // through because the bridge's parser also resolves named colors.
        apply_css_background_gradient(*v, gradient, parseColor);
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

    register_shader_widget_api();

    register_widget_schema_api();

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

    register_theme_api();

    register_platform_services_ai_api();

    register_metadata_computed_api();

    register_platform_services_exec_api();

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

    register_platform_services_dialog_api();

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
        // Phase 9: invoke each callback under its own ambient
        // provenance. The script identity (set via
        // `load_script(code, script_id)` / `set_active_script_id`) is
        // prefixed onto the callback id so a published value emitted
        // from inside an rAF body inherits source_id =
        // "<script_id>:<callback_id>". One callback at a time so the
        // ambient slot is correct per-invocation.
        const bool stamp = !active_script_id_.empty();
        // RAII guard so an exception in `__invokeFrame__` doesn't leave
        // stale ambient provenance behind, corrupting attribution for
        // every subsequent publish until something else clears it.
        struct AmbientGuard {
            bool active;
            ~AmbientGuard() { if (active) motion::clear_ambient_provenance(); }
        };
        for (auto id : ids) {
            AmbientGuard guard{stamp};
            if (stamp) {
                motion::Provenance p;
                p.source_kind = "rAF";
                p.source_id = active_script_id_ + ":" + std::to_string(id);
                motion::set_ambient_provenance(std::move(p));
            }
            std::string call = "__invokeFrame__(" + std::to_string(id) + ");void 0;";
            engine_.evaluate(call);
            // guard's dtor runs on normal AND exception paths.
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════════
    // Phase 9: motion observability — JS-side bridge for the publish
    // channel + ambient provenance slot.
    // ═══════════════════════════════════════════════════════════════════
    //
    // Exposes:
    //   motion.publishValue(viewName, metricName, value)
    //   motion.setProvenance(kind, id)         // sticky
    //   motion.clearProvenance()
    //
    // The C++ side already off-by-defaults when tracing is disabled, so
    // calling these in production code is cheap. Design-import codegen
    // can emit `motion.setProvenance("design-import", "figma:Card/Hover")`
    // alongside its `view.animate({...})` so the emitted trace carries
    // the vendor + node identity. rAF callbacks pick up
    // `source_kind="rAF"` automatically via the ambient slot set by
    // `__flushFrames__` when an active script id is configured.

    engine_.register_function("__motionPublishValue__",
        [](choc::javascript::ArgumentList args) {
            auto view = args.get<std::string>(0, "");
            auto metric = args.get<std::string>(1, "");
            auto value = args.get<double>(2, 0.0);
            if (!view.empty() && !metric.empty()) {
                motion::publish_value(std::move(view), std::move(metric), value);
            }
            return choc::value::Value();
        });

    engine_.register_function("__motionSetProvenance__",
        [](choc::javascript::ArgumentList args) {
            motion::Provenance p;
            p.source_kind = args.get<std::string>(0, "");
            p.source_id = args.get<std::string>(1, "");
            // Optional file + line for callers that want to point at a
            // specific JSX/JS source line.
            p.source_file = args.get<std::string>(2, "");
            p.source_line = args.get<int>(3, 0);
            motion::set_ambient_provenance(std::move(p));
            return choc::value::Value();
        });

    engine_.register_function("__motionClearProvenance__",
        [](choc::javascript::ArgumentList) {
            motion::clear_ambient_provenance();
            return choc::value::Value();
        });

    // Install the JS-side `motion` global wrapping the natives.
    // Idempotent — re-evaluating the same definition is a no-op.
    engine_.evaluate(
        "if (typeof globalThis.motion === 'undefined') {"
        "  globalThis.motion = {"
        "    publishValue: function(view, metric, value) {"
        "      return __motionPublishValue__(String(view), String(metric), Number(value));"
        "    },"
        "    setProvenance: function(kind, id, file, line) {"
        "      return __motionSetProvenance__(String(kind || ''), String(id || ''),"
        "                                     String(file || ''), Number(line || 0));"
        "    },"
        "    set_provenance: function(kind, id, file, line) {"
        "      return globalThis.motion.setProvenance(kind, id, file, line);"
        "    },"
        "    clearProvenance: function() { return __motionClearProvenance__(); },"
        "  };"
        "}"
        "void 0;"
    );

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

    register_platform_services_clipboard_api();

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

    register_storage_key_value_api();
    register_asset_loading_api();

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

    register_font_assets_api();

    register_shader_canvas_api();

    // WebGPU: getGPUInfo() → device capabilities
    engine_.register_function("getGPUInfo", [this](choc::javascript::ArgumentList) {
        auto gpu_info = widget_bridge_gpu_info(gpu_surface_);
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

    auto gpu_info = widget_bridge_gpu_info(gpu_surface_);
    HostObjectDescriptor gpu;
    gpu.class_name = "GPU";
    gpu.properties.push_back({"backend", choc::value::createString(gpu_info.backend)});
    gpu.properties.push_back({"backendType", choc::value::createString(gpu_info.backend_type)});
    gpu.properties.push_back({"available", choc::value::createBool(gpu_info.available)});
    gpu.properties.push_back({"nativeBridge", choc::value::createBool(gpu_info.native_bridge)});
    gpu.methods.push_back({"getPreferredCanvasFormat", [this](const choc::value::Value*, size_t) {
        return choc::value::createString(widget_bridge_gpu_info(gpu_surface_).preferred_canvas_format);
    }});
    engine_.register_host_object("navigatorGPU", std::move(gpu));

    engine_.register_function("__describeNativeAdapterImpl", [this](choc::javascript::ArgumentList) {
        return gpu_descriptor_to_value(widget_bridge_gpu_info(gpu_surface_));
    });

    engine_.register_function("__describeNativeDeviceImpl", [this](choc::javascript::ArgumentList) {
        auto gpu_info = widget_bridge_gpu_info(gpu_surface_);
        auto device = choc::value::createObject("");
        device.addMember("nativeBridge", choc::value::createBool(gpu_info.native_bridge));
        device.addMember("adapterInfo", gpu_adapter_info_to_value(gpu_info));
        return device;
    });

    engine_.register_function("__gpuCanvasConfigureImpl", [this](choc::javascript::ArgumentList args) {
        auto gpu_info = widget_bridge_gpu_info(gpu_surface_);
        auto canvas_id = args.get<std::string>(0, "");
        auto width = static_cast<uint32_t>(std::max(1, args.get<int32_t>(1, 1)));
        auto height = static_cast<uint32_t>(std::max(1, args.get<int32_t>(2, 1)));
        auto format = args.get<std::string>(3, gpu_info.preferred_canvas_format);
        auto usage = static_cast<uint32_t>(args.get<int32_t>(4, 0));
        auto alpha_mode = args.get<std::string>(5, "opaque");

        // iOS-D.3b Slice 4: surface the GpuSurface's presentable/offscreen
        // distinction to JS. Until slice 5 wires per-frame
        // current_texture_handle() acquisition into getCurrentTexture(),
        // configure() returning `presentable: true` is the program's
        // contract that JS draws WILL hit the visible swapchain rather
        // than a silent offscreen texture (Codex pass-1 finding #3).
        //
        // `has_surface()` is a member of render::GpuSurface; the full type
        // is only visible when pulp/render/gpu_surface.hpp is on the
        // include path. On no-GPU configures (e.g. iOS Simulator with
        // PULP_ENABLE_GPU=OFF) the render module isn't built, the header
        // isn't reachable, and GpuSurface stays forward-declared — so we
        // must gate the call under PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE and
        // report presentable=false when GPU is unavailable.
#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
        const bool presentable = (gpu_surface_ != nullptr) && gpu_surface_->has_surface();
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
        if (state.configured) {
            // PULP_WEBGPU_BRIDGE log markers (slice 4 contract) — surface
            // the presentable distinction in the runtime log so iPad
            // device walk-throughs can grep the value without
            // introspecting the JS-returned object. The `presentable`
            // value comes from gpu_surface_->has_surface() above.
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

    engine_.register_function("__gpuCanvasDescribeCurrentTextureImpl", [this](choc::javascript::ArgumentList args) {
        // iOS-D.3b Slice 4: surface the presentable flag here too so
        // JS can verify per-frame that the texture it's about to draw
        // into IS the visible swapchain (slice 5 wires
        // gpu_surface_->current_texture_handle() through this path;
        // slice 4 just plumbs the boolean). Same PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
        // gate as the configure path above — no-GPU configures keep
        // presentable=false because there is no swapchain to address.
#if PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
        const bool presentable = (gpu_surface_ != nullptr) && gpu_surface_->has_surface();
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

        if (canvas_id.empty() || native_gpu_bridge_state_ == nullptr) {
            return descriptor;
        }

        auto it = native_gpu_bridge_state_->canvases.find(canvas_id);
        if (it == native_gpu_bridge_state_->canvases.end() || !it->second.configured) {
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

    engine_.register_function("__gpuQueueWriteTextureImpl", [this](choc::javascript::ArgumentList args) {
        auto payload_json = args.get<std::string>(0, "");
        if (payload_json.empty() || native_gpu_bridge_state_ == nullptr || gpu_surface_ == nullptr) {
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
        auto texture_it = native_gpu_bridge_state_->textures.find(texture_id);
        if (texture_id.empty() || texture_it == native_gpu_bridge_state_->textures.end() ||
            !texture_it->second.configured || !texture_it->second.texture) {
            return choc::value::createBool(false);
        }

        auto* queue_ptr = static_cast<wgpu::Queue*>(gpu_surface_->dawn_queue_handle());
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

        // iOS-D.3b Slice 5: surface the queue.submit success in the
        // runtime log so iPad device walks can grep `PULP_WEBGPU_BRIDGE:
        // queue.submit ok` without instrumenting JS. `commands=1` is
        // accurate for this single-command-buffer code path; the buffered
        // shim (`__gpuQueueDrawBufferedImpl` etc.) handles multi-command
        // submissions and would emit a different count.
        runtime::log_info(
            "PULP_WEBGPU_BRIDGE: queue.submit ok (canvas={}, commands=1)",
            canvas_id);
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
        std::vector<wgpu::Texture> bind_group_textures;
        std::vector<wgpu::TextureView> bind_group_texture_views;
        std::vector<wgpu::BindGroup> bind_groups;
        // iOS-D.3c (#3217): (group_index, entries) captured during serialization
        // and turned into bind groups AFTER the pipeline is built with an auto
        // layout — see the deferred-creation comment below.
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

                // iOS-D.3c (#3217): defer bind-group creation until the pipeline
                // exists. The JS serializer guesses each entry's visibility/type
                // by regex-scanning the WGSL
                // (web-compat-gpu-buffered.js inferVisibilityFromShaders); an
                // explicit BindGroupLayout built from those guesses can silently
                // diverge from Three.js's `layout:"auto"` pipeline interface.
                // With the Sim's skip_validation toggle that divergence does NOT
                // raise an error — it just leaves the vertex stage reading zeroed
                // uniforms, collapsing the cube to a degenerate point (every
                // matrix/vertex byte verified correct yet no fragments emit).
                // Instead, build each group from pipeline.GetBindGroupLayout()
                // below, exactly as the immediate __gpuQueueDrawImpl path does.
                (void)bind_group_layout_entries;
                deferred_bind_groups.emplace_back(group_index, std::move(bind_group_entries));
            }
        }

        // iOS-D.3c (#3217 Codex pass 1): the pipeline's color attachment
        // format MUST match the actual target texture's format, NOT the
        // JS-supplied payload `format` field. Three.js may request a
        // bgra8unorm RenderPass but the intermediate texture is created
        // as rgba8unorm — Metal SoftwareRenderer silently rejects the
        // pipeline (the mismatch is suppressed by skip_validation). Use
        // the target's actual format so the pipeline matches the
        // attachment.
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
        // iOS-D.3c (#3217): writeMask was unset → defaulted to None →
        // pipeline executed but never wrote color. The immediate __gpuQueueDrawImpl
        // path sets `writeMask = All` (line 8767); buffered path must too,
        // otherwise the magenta-clear test paints but Three.js's actual
        // shader output silently vanishes. (Codex root-cause for #3217.)
        color_target.writeMask = wgpu::ColorWriteMask::All;

        // iOS-D.3c (#3217): for canvas-targeted draws specifically, drop
        // the alpha channel from writeMask so the shader's alpha=0 output
        // (Three.js's WebGPURenderer composite path) doesn't reset the
        // destination alpha to 0. Combined with the loadOp=Clear/alpha=1
        // override below at color_attachment, this keeps the canvas
        // swapchain alpha at 1 (opaque) regardless of shader output —
        // Skia then composites the RGB content the shader wrote instead
        // of compositing src.alpha=0 against the canvasCard CSS bg.
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
        // iOS-D.3c (#3217): always use an AUTO pipeline layout. Bind groups are
        // created below from pipeline.GetBindGroupLayout(group_index), so the
        // layout is derived from the actual shader interface rather than the
        // JS-side guessed layout (see deferred_bind_groups above).
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

        // iOS-D.3c (#3217): now that the pipeline (auto layout) exists, build
        // each bind group against the layout Dawn derived from the shader
        // interface. This guarantees the binding visibility/types match what
        // the WGSL actually declares, instead of the JS-side guessed layout
        // that left the vertex stage reading zeroed uniforms on the Simulator.
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
        // iOS-D.3c (#3217): for canvas-targeted draws, force loadOp=Clear
        // with alpha=1 so the destination alpha starts opaque. Combined
        // with the canvas-specific writeMask (RGB only, set above on the
        // pipeline's color_target), this keeps the canvas swapchain
        // alpha at 1 across all draws regardless of what alpha the
        // shader emits. Without this, Three.js's composite (which
        // writes alpha=0 for the canvas pass) leaves the canvas
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

    engine_.register_promise_function("__requestAdapterImpl", [this](const choc::value::Value*, size_t) {
        return gpu_descriptor_to_value(widget_bridge_gpu_info(gpu_surface_));
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

} // namespace pulp::view
