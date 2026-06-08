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

    register_animation_api();

    register_widget_style_visibility_api();

    register_accessibility_api();

    register_hover_event_api();
    register_layout_grid_api();

    register_pointer_event_api();

    register_metadata_removal_api();

    // ═══════════════════════════════════════════════════════════════
    // Extended API: containers, layout, all widgets, themes, canvas
    // ═══════════════════════════════════════════════════════════════

    register_widget_factory_container_api();
    register_layout_flex_api();
    register_dom_api();
    register_layout_query_api();

    register_widget_style_interaction_api();

    register_widget_factory_composite_api();

    register_widget_value_list_api();

    register_widget_factory_text_editor_api();



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

    register_widget_border_radius_api();

    register_widget_border_side_api(parseHexColor);

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

    register_wheel_event_api();

    engine_.register_function("setOpacity", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto alpha = args.get<double>(1, 1.0);
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_opacity((float)alpha);
        return choc::value::Value();
    });

    register_widget_typography_color_api(parseHexColor);

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

    register_canvas2d_api(parseColor);


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

    register_widget_typography_decoration_api(parseHexColor);

    register_widget_style_background_repeat_api();
    register_layout_position_api();

    register_animation_style_api();

    register_widget_typography_overflow_api();

    register_widget_style_cursor_direction_api();

    register_widget_style_filter_clip_api(parseColor);

    register_widget_style_mask_object_api();

    register_widget_style_blend_api();

    // pulp #1434 A4 Bundles 5–7 closure — storage-only setters for the
    // remaining css NOT-IMPL entries. Each handler records the value on
    // the View's catalog slot so harness round-trip tests can verify
    // the bridge accepts the keyword. Catalog status documents the
    // implementation depth (`partial` for storage-only with deferred
    // paint, `noop` for accept-and-ignore, `wontfix` for architectural
    // out-of-scope).

    register_widget_typography_extended_api();

    register_widget_style_rn_compat_api(parseHexColor);

    register_widget_typography_shadow_shorthand_api();

    register_widget_style_background_subproperty_api();

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


    register_shader_widget_api();

    register_widget_schema_api();


    register_theme_api();

    register_platform_services_ai_api();

    register_metadata_computed_api();

    register_platform_services_exec_api();

    register_context_menu_event_api();

    register_platform_services_dialog_api();

    register_runtime_api();

    register_platform_services_clipboard_api();


    register_storage_key_value_api();
    register_asset_loading_api();


    register_drop_event_api();

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
