#include <pulp/view/widget_bridge.hpp>
#include "widget_bridge/gpu_common.hpp"
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/modal.hpp>
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

namespace pulp::view {

namespace {

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

// `on(id, eventName, fn)` is the JS-side hook callers use to register event
// callbacks. Storing the function in __callbacks__ is necessary but not
// sufficient: events fired through View::on_click, on_hover_enter, or
// on_pointer_event require an explicit `registerClick(id)`, `registerHover(id)`,
// or `registerPointer(id)` to wire the native View callback. Without that
// wiring, real platform clicks reach View::on_mouse_down/up but never trigger
// __dispatch__('click'), so the JS handler never runs.
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
//
// Fan out events targeting the synthetic '__global__' id into
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
        // Keep handler exceptions inside the JS dispatch boundary. If a React
        // handler throws out of evaluate(), it can unwind the C++ caller and
        // kill requestAnimationFrame's self-rescheduling chain. Surface the
        // error via __dispatchError__ if defined so handlers can throw without
        // halting the frame loop.
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
    // Fan out to document-level listeners. React click-outside patterns
    // (`document.addEventListener('mousedown', onDoc)`) are the common
    // close-the-popover idiom; the framework fires synthetic outside-click
    // events on Esc / overlay dismiss through this path, so any React popover
    // using the pattern closes without per-app wiring.
    if (id === 'document' && typeof document !== 'undefined' && document.dispatchEvent) {
        var devt = args && args.length ? args[0] : {};
        if (devt && typeof devt === 'object') {
            devt.type = eventName;
            document.dispatchEvent(devt);
        }
    }
    // For pointer/mouse/click events, also call dispatchEvent on the JS Element
    // for `id`. This bypasses the single-slot __callbacks__[id:event] table
    // that _registerNativeEvent can clobber when React-DOM calls
    // element.addEventListener(...). Without this, native platform events fire
    // the low-level on() callback but never re-enter the DOM bubble walk where
    // React-DOM's delegated root listener lives.
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
                // Verbose dispatch log is gated on __spectrLog being present.
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
        // Wheel subscriptions route through on(id, 'wheel', fn). Without this
        // case the JS callback is stored but registerWheel(id) is never
        // invoked, so wheel events never reach the JS handler. This is
        // critical for trackpad zoom on any wrapper div that subscribes via
        // 'wheel'.
        __ensureNativeRegistered__(id, 'wheel');
    }
}
)";

// Window event-listener shim installed after the web-compat-document.js prelude
// because that prelude re-declares `var window = {...}` and would clobber any
// listener hooks installed earlier. This is the minimal install needed for
// `__dispatch__('__global__', 'keydown', ...)` fan-out to reach
// `window.addEventListener('keydown', fn)` listeners. Idempotent via the
// `__pulpListenerShim__` marker, so re-eval is safe.
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

// DOM mutation methods live in core/view/js/web-compat-dom-ops.js. That JS file
// is the single source of truth and gets evaluated by the constructor along
// with the rest of the prelude chain.

static void safe_dispatch_eval(ScriptEngine& engine, const std::string& js, const char* context) {
    try {
        engine.evaluate(js);
        // Pump microtasks so React setState commits (and any queueMicrotask /
        // Promise.then continuations scheduled by the handler) before the next
        // event arrives. Without this, drag-style interactions see stale state
        // on the immediately-following pointermove and silently bail.
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
        // on the immediately-following pointermove and silently bail.
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
// recursive_mutex (not plain mutex): the `dispatch_*` helpers below hold the
// lock for the full fan-out, including the call into each bridge's
// `forward_key_event` / JS evaluation. A snapshot-then-unlock pattern can UAF if
// a bridge is destroyed on another thread mid-iteration. Holding during dispatch
// is safe: the standard bridge ctor/dtor lifecycle is host-driven, and
// recursive_mutex defensively tolerates same-thread reentry.
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
                           render::GpuSurface* gpu_surface,
                           CapabilitySet granted_capabilities)
    : engine_(engine), root_(root), store_(store),
      granted_capabilities_(granted_capabilities), gpu_surface_(gpu_surface) {
    {
        std::lock_guard<std::recursive_mutex> lock(all_bridges_mutex());
        all_bridges_set().insert(this);
    }
    if (detail::widget_bridge_gpu_info(gpu_surface_).native_bridge) {
        native_gpu_bridge_state_ = std::make_unique<NativeGpuBridgeState>();
    }
    // Default repaint wiring: route to the root view's host invalidator.
    // Without this, JS `requestAnimationFrame` callbacks queue but never
    // schedule a second paint, because the only way out of `request_repaint()`
    // is `repaint_callback_`. Hosts that need a custom invalidator (the
    // standalone window's top-level editor) replace this via
    // `set_repaint_callback`.
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
    // Events + pointer-capture helpers must eval after web_compat_element so
    // the Element constructor and prototype exist before overrides install.
    eval_or_throw(engine_, "web_compat_element_events", preludes::web_compat_element_events);
    eval_or_throw(engine_, "web_compat_canvas", preludes::web_compat_canvas);
    // Native GPU canvas helpers must eval after the canvas core so
    // CanvasRenderingContext2D and window.pulp.gpu are in scope when
    // __ensurePulpGpuHelpers / getContext("webgpu") run.
    eval_or_throw(engine_, "web_compat_canvas_gpu", preludes::web_compat_canvas_gpu);
    // _PulpCanvasMatrix DOMMatrix-compat helper plus Canvas2D API closures
    // must eval after web_compat_canvas so the CanvasRenderingContext2D
    // constructor and prototype are in scope.
    eval_or_throw(engine_, "web_compat_canvas_matrix", preludes::web_compat_canvas_matrix);
    eval_or_throw(engine_, "web_compat_canvas_image", preludes::web_compat_canvas_image);
    eval_or_throw(engine_, "web_compat_style_decl", preludes::web_compat_style_decl);
    // Per-domain `_applyProperty` handler modules. The property switch is split
    // into layout / paint / typography / transform / misc handlers;
    // web-compat-style-decl.js's `_applyProperty` is a thin dispatcher that
    // resolves those function declarations at call time. Evaluate handlers
    // after style_decl and before any consumer can trigger a style apply.
    eval_or_throw(engine_, "web_compat_style_decl_layout", preludes::web_compat_style_decl_layout);
    eval_or_throw(engine_, "web_compat_style_decl_paint", preludes::web_compat_style_decl_paint);
    eval_or_throw(engine_, "web_compat_style_decl_typography", preludes::web_compat_style_decl_typography);
    eval_or_throw(engine_, "web_compat_style_decl_transform", preludes::web_compat_style_decl_transform);
    eval_or_throw(engine_, "web_compat_style_decl_misc", preludes::web_compat_style_decl_misc);
    // CSSStyleDeclaration helper installation needs the style constructor and
    // _applyProperty prototype before it walks __cssProperties__.
    eval_or_throw(engine_, "web_compat_style_decl_helpers", preludes::web_compat_style_decl_helpers);
    eval_or_throw(engine_, "web_compat_animation", preludes::web_compat_animation);
    eval_or_throw(engine_, "web_compat_document", preludes::web_compat_document);
    // CSS selector engine must eval after document and Element so the
    // selector helpers are resolvable when document.querySelector /
    // .querySelectorAll dispatch into them.
    eval_or_throw(engine_, "web_compat_document_selectors", preludes::web_compat_document_selectors);
    // WebGPU mock factories must eval after document so GPU* usage constants
    // are in scope when factory bodies resolve them lazily at call time.
    eval_or_throw(engine_, "web_compat_document_gpu_mock", preludes::web_compat_document_gpu_mock);
    eval_or_throw(engine_, "web_compat_gpu_buffered", preludes::web_compat_gpu_buffered);
    // DOM mutation methods (appendChild / removeChild / etc.). Single source
    // of truth lives in core/view/js/web-compat-dom-ops.js.
    // The JS file's idempotency guard (`__pulp_dom_ops__` marker on the
    // prototype methods) makes a re-eval a no-op, which matters because
    // load_script callers used to re-trigger this initialization manually
    // before the consolidation.
    eval_or_throw(engine_, "web_compat_dom_ops", preludes::web_compat_dom_ops);
    // Observer no-ops and scheduler shims so React 18, and any other framework
    // that feature-detects MutationObserver / MessageChannel / queueMicrotask,
    // finds the constructors it expects on the global.
    eval_or_throw(engine_, "web_compat_observers", preludes::web_compat_observers);
    eval_or_throw(engine_, "web_compat_scheduler", preludes::web_compat_scheduler);
    // DOM construction and walker for the Claude Design `--execute-bundle`
    // import lane. Hides itself behind `globalThis.__pulpImportRuntime__` so
    // non-import scripts do not see new globals.
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

// Late-attach of the GpuSurface for the common case where ScriptedUiSession /
// ViewBridge is constructed before the PluginViewHost and therefore before the
// surface exists. Mirrors the fourth constructor argument and is idempotent.
void WidgetBridge::attach_gpu_surface(render::GpuSurface* gpu_surface) {
    if (gpu_surface_ == gpu_surface) return;
    gpu_surface_ = gpu_surface;
    const auto gpu_info = detail::widget_bridge_gpu_info(gpu_surface_);
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
    safe_dispatch_eval(engine_, detail::gpu_host_object_update_script(gpu_info), "gpu surface attach");
}

bool WidgetBridge::has_native_gpu_bridge() const noexcept {
    return gpu_surface_ != nullptr && native_gpu_bridge_state_ != nullptr;
}

void WidgetBridge::dispatch_global_key(int key_code, uint16_t modifiers, bool is_down) {
    // Hold the registry lock for the entire fan-out. Copying raw pointers
    // under lock and then iterating unlocked can UAF if a bridge is destroyed
    // on another thread between snapshot and dispatch. recursive_mutex
    // tolerates same-thread reentry.
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
    // Same lifetime-safety rationale as dispatch_global_key above: hold the
    // lock through iteration.
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
    // DOM mutation methods are installed by the constructor's prelude chain
    // (`web_compat_dom_ops`). The JS-side idempotency guard makes re-eval a
    // no-op, so callers that load multiple scripts do not need bridge-local
    // "first time" state.
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
    // Record script identity before eval so requestAnimationFrame calls made
    // during eval already see the new active script. Cleared by the caller, or
    // by a subsequent load_script call, when the script's surface goes away.
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
        // HTML <input type="range"> change event. The payload is the
        // post-quantisation value, not normalized, so JS callers see the same
        // number they handed us via setValue/setMin/setMax/setStep.
        r->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "range slider change");
        };
    } else if (auto* c = dynamic_cast<ComboBox*>(w)) {
        // Mirror createCombo's inline wiring so a `<combo>`/`<select>` tag
        // routed through __domAppend dispatches the same `select` event as the
        // factory path.
        c->on_change = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "combo select");
        };
    } else if (auto* cb = dynamic_cast<Checkbox*>(w)) {
        // Mirror createCheckbox's inline `change` wiring.
        cb->on_change = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::string(v?"1":"0") + ")", "checkbox change");
        };
    } else if (auto* lb = dynamic_cast<ListBox*>(w)) {
        // Mirror createListBox's inline select/activate wiring.
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
    param_bindings_.clear();  // bindings reference widgets torn down below
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
        // Selection controls: their selected INDEX is reload state too — without
        // this a dev reload silently resets the user's dropdown / segment choice
        // (item 1.4 coverage gap). Stored as the index-as-float in scalar_values;
        // restore keys by id so the type is unambiguous.
        else if (auto* combo = dynamic_cast<ComboBox*>(view)) out.scalar_values[id] = static_cast<float>(combo->selected());
        else if (auto* seg = dynamic_cast<SegmentedControl*>(view)) out.scalar_values[id] = static_cast<float>(seg->selected());
        else if (auto* xy = dynamic_cast<XYPad*>(view)) out.xy_values[id] = {.x = xy->x_value(), .y = xy->y_value()};
        // Custom widget-declared state (item 1.4b): independent of the built-in
        // type match above — a custom widget opts in via save_reload_state.
        // Built-in widgets return false (View default), so this stays empty for
        // them and adds no per-widget cost beyond one virtual call.
        std::string blob;
        if (view->save_reload_state(blob)) out.custom_state[id] = std::move(blob);
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
        // Selection controls — restore the index SILENTLY so re-applying it during
        // a reload doesn't fire the widget's on_change as if the user clicked.
        else if (auto* combo = dynamic_cast<ComboBox*>(it->second)) combo->set_selected_silent(static_cast<int>(std::lround(val)));
        else if (auto* seg = dynamic_cast<SegmentedControl*>(it->second)) seg->set_selected_silent(static_cast<int>(std::lround(val)));
    }
    for (auto& [id, val] : snapshot.xy_values) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        if (auto* xy = dynamic_cast<XYPad*>(it->second)) { xy->set_x(val.x); xy->set_y(val.y); }
    }
    // Custom widget-declared state (item 1.4b): hand each saved blob back to the
    // widget that still lives under the same script id. A widget whose id/type
    // changed across the reload simply won't match (find fails or the virtual
    // ignores an unfamiliar blob) — fail-safe, no throw.
    for (auto& [id, blob] : snapshot.custom_state) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        (void)it->second->restore_reload_state(blob);
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
    // Declarative bindings first: pure C++ store→widget push, no JS crossing.
    service_param_bindings();
    engine_.pump_message_loop();
    // Drain any expired native-tracked setTimeout/setInterval timers so
    // consumers do not need a JS shim around the frame loop.
    if (!pending_timers_.empty()) {
        engine_.evaluate("if (typeof __flushTimers__ === 'function') __flushTimers__();void 0");
        engine_.pump_message_loop();
    }
    if (!pending_frame_ids_.empty()) {
        engine_.evaluate("if (typeof __flushFrames__ === 'function') __flushFrames__();void 0");
        engine_.pump_message_loop();
    }
}

#ifndef PULP_BRIDGE_EXEC_ENABLED
// Shell exec/execAsync is a development-only affordance. Compiled out of shipping
// builds via -DPULP_BRIDGE_EXEC_ENABLED=0, and even when compiled in it is ALSO
// gated behind the `exec` capability, so a UI with no granted capabilities never
// receives it. Two independent locks, because shelling out is the sharpest edge.
#define PULP_BRIDGE_EXEC_ENABLED 1
#endif

void WidgetBridge::register_api() {
    register_widget_factory_controls_api();
    // Reads arbitrary file paths (image/sprite assets) → gated by Filesystem.
    if (granted_capabilities_.has(ReloadCapability::Filesystem))
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

    register_widget_factory_design_system_api();



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

    register_widget_style_background_color_api(parseHexColor);

    register_svg_api(parseHexColor);

    register_widget_border_box_api(parseHexColor);

    register_list_style_api();

    register_widget_outline_api(parseHexColor);

    register_widget_border_radius_api();

    register_widget_border_side_api(parseHexColor);

    register_widget_style_shadow_api(parseHexColor);

    register_wheel_event_api();

    register_widget_style_opacity_api();

    register_widget_typography_color_api(parseHexColor);

    register_widget_style_overflow_api();
    register_layout_box_model_api();

    register_canvas2d_api(parseColor);

    register_widget_style_state_api(parseColor);

    register_widget_typography_decoration_api(parseHexColor);

    register_widget_style_background_repeat_api();
    register_layout_position_api();

    register_animation_style_api();

    register_widget_typography_overflow_api();

    register_widget_style_cursor_direction_api();

    register_widget_style_filter_clip_api(parseColor);

    register_widget_style_mask_object_api();

    register_widget_style_blend_api();

    // Storage-only setters for remaining CSS compatibility entries. Each
    // handler records the value on the View's catalog slot so harness
    // round-trip tests can verify the bridge accepts the keyword. Catalog
    // status documents implementation depth: `partial` for storage-only with
    // deferred paint, `noop` for accept-and-ignore, and `wontfix` for
    // architectural out-of-scope.

    register_widget_typography_extended_api();

    register_widget_style_rn_compat_api(parseHexColor);

    register_widget_typography_shadow_shorthand_api();

    register_widget_style_background_subproperty_api();

    register_widget_style_background_gradient_api(parseColor);
    register_widget_style_box_shadow_api(parseHexColor);


    register_shader_widget_api();

    // Persists style presets to temp files → gated by Storage.
    if (granted_capabilities_.has(ReloadCapability::Storage))
        register_widget_schema_api();


    register_theme_api();

    if (granted_capabilities_.has(ReloadCapability::Ai))
        register_platform_services_ai_api();

    register_metadata_computed_api();

    // Shell exec — dev-only (compiled out of ship) AND gated by the exec cap.
#if PULP_BRIDGE_EXEC_ENABLED
    if (granted_capabilities_.has(ReloadCapability::Exec))
        register_platform_services_exec_api();
#endif

    register_context_menu_event_api();

    // Native file dialogs read/choose arbitrary paths → gated by Filesystem.
    if (granted_capabilities_.has(ReloadCapability::Filesystem))
        register_platform_services_dialog_api();

    register_runtime_api();

    if (granted_capabilities_.has(ReloadCapability::Clipboard))
        register_platform_services_clipboard_api();


    // Key/value persistence → Storage; asset loading reads file:// paths →
    // Filesystem (granted independently).
    if (granted_capabilities_.has(ReloadCapability::Storage))
        register_storage_key_value_api();
    if (granted_capabilities_.has(ReloadCapability::Filesystem))
        register_asset_loading_api();


    register_drop_event_api();

    register_font_assets_api();

    register_shader_canvas_api();

    register_gpu_api();

}

} // namespace pulp::view
