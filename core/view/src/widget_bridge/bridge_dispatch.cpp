#include "bridge_dispatch.hpp"

#include "js_trace_scopes.hpp"

#include <pulp/runtime/trace.hpp>

#include <choc/text/choc_JSON.h>

#include <exception>
#include <iostream>

namespace pulp::view {

namespace {

// MSVC caps a single string literal at 16380 bytes (C2026), and the ARM64
// cross-compiler enforces the cap where the x64 host compiler does not, so an
// over-long literal breaks only the windows-arm64 build. The dispatch prelude
// is therefore stored in parts joined by dispatch_prelude_js(); keep each part
// well under the cap when adding to it.
constexpr const char* kDispatchPrelude = R"(
var __callbacks__ = {};
var __nativeRegistered__ = {};
var __pulpEventPropagation__ = {};
var __pulpEventPropagationOrder__ = [];

function __pulpPropagationKey__(eventName, args) {
    var data = args && args.length ? args[0] : null;
    var token = data && typeof data === 'object'
        ? data.__pulpDispatchToken
        : 0;
    return (typeof token === 'number' && token > 0)
        ? String(token) + ':' + eventName
        : '';
}

function __pulpPropagationLevel__(result) {
    var level = result && typeof result === 'object'
        ? result.__pulpEventPropagation
        : 0;
    return level === 2 ? 2 : level === 1 ? 1 : 0;
}

function __pulpRememberPropagation__(key, level) {
    if (!key || level <= 0) return;
    if (!Object.prototype.hasOwnProperty.call(__pulpEventPropagation__, key)) {
        __pulpEventPropagationOrder__.push(key);
    }
    if ((__pulpEventPropagation__[key] || 0) < level) {
        __pulpEventPropagation__[key] = level;
    }
    // Native dispatch tokens are monotonic, so old entries can never affect a
    // later delivery. Bound retained state for long-running editor sessions.
    while (__pulpEventPropagationOrder__.length > 128) {
        delete __pulpEventPropagation__[__pulpEventPropagationOrder__.shift()];
    }
}
//
// Fan out events targeting the synthetic '__global__' id into
// window._listeners[eventName] — `window.addEventListener('keydown',
// fn)` is the standard DOM API, but without this fan-out the native
// keydown only fed __callbacks__['__global__:keydown'] (a per-id
// channel nothing subscribes to). With it, Spectr-style global key
// listeners just work.
function __pulpDispatchCallbackOnlyUnbatched__(id, eventName) {
    var args = Array.prototype.slice.call(arguments, 2);
    var propagationKey = __pulpPropagationKey__(eventName, args);
    var priorLevel = propagationKey
        ? (__pulpEventPropagation__[propagationKey] || 0)
        : 0;
    if (priorLevel > 0) return priorLevel;
    var key = id + ':' + eventName;
    var cb = __callbacks__[key];
    // The React host keeps the current closure in its own registry so a
    // retained DOM/materialized node can survive native subtree replacement.
    // If native callback cleanup ran during that replacement, recover the
    // live handler from the authoritative React registry instead of dropping
    // the first semantic event delivered before listener replay.
    if (!cb && typeof globalThis !== 'undefined'
        && globalThis.__pulpReactEventCallbacks__ instanceof Map) {
        cb = globalThis.__pulpReactEventCallbacks__.get(key);
    }
    var level = 0;
    if (cb) {
        // Keep handler exceptions inside the JS dispatch boundary. If a React
        // handler throws out of evaluate(), it can unwind the C++ caller and
        // kill requestAnimationFrame's self-rescheduling chain. Surface the
        // error via __dispatchError__ if defined so handlers can throw without
        // halting the frame loop.
        try { level = __pulpPropagationLevel__(cb.apply(null, args)); }
        catch (e) {
            if (typeof __dispatchError__ === 'function') __dispatchError__(id, eventName, String(e && e.stack ? e.stack : e));
        }
    }
    __pulpRememberPropagation__(propagationKey, level);
    return level;
}
function __pulpDispatchUnbatched__(id, eventName) {
    var args = Array.prototype.slice.call(arguments, 2);
    var callbackPropagation = __pulpDispatchCallbackOnlyUnbatched__.apply(null, arguments);
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
        // Materialized-state upgrades may replace a native View subtree while
        // retaining the corresponding live DOM nodes. The cleanup correctly
        // retires stale callbacks, but older upgrade paths can temporarily
        // drop the native-id map entry before the retained node is appended
        // again. Recover by identity from the live document so semantic
        // events such as `dismiss` are never lost during that bounded seam.
        if (!el && typeof document !== 'undefined' && document.querySelectorAll) {
            var liveNodes = document.querySelectorAll('*');
            for (var ni = 0; ni < liveNodes.length; ++ni) {
                if (liveNodes[ni] && liveNodes[ni]._id === id) {
                    el = liveNodes[ni];
                    __nativeElements__[id] = el;
                    break;
                }
            }
        }
        if (typeof globalThis.__pulpDispatchHits__ === 'undefined') globalThis.__pulpDispatchHits__ = { byType: {}, missingElement: 0, dispatched: 0, rootListenersFired: 0, lastErr: null, total: 0 };
        var stats = globalThis.__pulpDispatchHits__;
        stats.total = (stats.total || 0) + 1;
        if (el && el.dispatchEvent &&
            /^(pointerdown|pointermove|pointerup|pointercancel|click|mousedown|mousemove|mouseup|wheel|dismiss)$/.test(eventName)) {
            stats.byType[eventName] = (stats.byType[eventName] || 0) + 1;
            try {
                var data = args && args.length ? args[0] : {};
                // Older wheel producers supplied positional (dx, dy) values.
                // Normalize them here, at the sole DOM dispatch path, rather
                // than dispatching once from the registered callback and once
                // again from this generic element fan-out.
                if (eventName === 'wheel' && (typeof data !== 'object' || data === null)) {
                    data = { deltaX: data || 0, deltaY: (args && args.length > 1 ? args[1] : 0) || 0 };
                }
                var ev = (typeof _makeEvent === 'function')
                    ? _makeEvent(eventName, el, data || {})
                    : {
                        type: eventName, target: el, currentTarget: el,
                        bubbles: true, cancelable: true,
                        clientX: (data && data.clientX) || 0,
                        clientY: (data && data.clientY) || 0,
                        button: (data && data.button) || 0,
                        buttons: (data && data.buttons !== undefined) ? data.buttons
                            : (eventName === 'pointerup' || eventName === 'mouseup') ? 0 : 1,
                        pointerId: (data && data.pointerId) || 0,
                        pointerType: (data && data.pointerType) || 'mouse',
                        isPrimary: true,
                        preventDefault: function () { this.defaultPrevented = true; },
                        stopPropagation: function () { this._stopped = true; },
                        stopImmediatePropagation: function () {
                            this._stopped = true;
                            this._stoppedImmediate = true;
                        }
                    };
                // A direct @pulp/react handler runs before this web-compat
                // target fan-out. stopPropagation still permits other target
                // listeners; stopImmediatePropagation suppresses them too.
                if (callbackPropagation === 1) ev._stopped = true;
                else if (callbackPropagation === 2) {
                    ev._stopped = true;
                    ev._stoppedImmediate = true;
                }
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
                if (callbackPropagation !== 2) {
                    el.dispatchEvent(ev);
                    stats.dispatched = (stats.dispatched || 0) + 1;
                }
                var propagationKey = __pulpPropagationKey__(eventName, args);
                var observedPropagation = callbackPropagation;
                if (ev._stoppedImmediate) observedPropagation = 2;
                else if (ev._stopped && observedPropagation < 1) observedPropagation = 1;
                __pulpRememberPropagation__(propagationKey, observedPropagation);
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
                // Only pointer events are fanned; the separately delivered
                // mouse event stays element-only to preserve document-mouse
                // semantics.
                if (typeof document !== 'undefined' && document.dispatchEvent &&
                    !ev._stopped &&
                    /^(pointerdown|pointermove|pointerup|pointercancel)$/.test(eventName)) {
                    document.dispatchEvent(ev);
                }
                // Pointer registrars emit the corresponding mouse event as a
                // separate bridge dispatch so direct @pulp/react listeners and
                // web-compat share one source. Do not synthesize it again here.
            } catch (e) {
                stats.lastErr = String(e && e.stack ? e.stack.slice(0, 200) : e);
                if (typeof __dispatchError__ === 'function') __dispatchError__(id, eventName, String(e && e.stack ? e.stack : e));
            }
        } else if (!el) {
            stats.missingElement = (stats.missingElement || 0) + 1;
        }
    }
}
)";

constexpr const char* kDispatchBatchPrelude = R"(
// A native widget event is one commit boundary, as a frame or timer callback
// is: everything it reaches (the direct handler, the window/document fan-out and
// the DOM bubble walk) runs inside the UI framework's batching hook, so the
// several state updates a click handler makes commit once, when the outermost
// dispatch returns. A dispatch made from inside a handler joins the batch that
// is already open instead of opening its own. __pulpRunHostCallback__ belongs
// to the runtime API registrar, which a view-core-only consumer may not
// install; without it the handlers run unbatched.
//
// When the outermost dispatch's batch has committed, the UI framework's
// __pulpAfterDiscreteDispatch__ hook runs, so it sees the props the handlers'
// state produced. A controlled control uses it to put a rejected edit back.
var __pulpDispatchDepth__ = 0;
function __pulpRunDispatchBatch__(body, args) {
    var outermost = __pulpDispatchDepth__ === 0;
    var result;
    ++__pulpDispatchDepth__;
    try {
        if (outermost && typeof __pulpRunHostCallback__ === 'function')
            __pulpRunHostCallback__(function() { result = body.apply(null, args); });
        else
            result = body.apply(null, args);
    } catch (e) {
        // The batch commits as the hook returns, outside the handlers' own
        // try/catch, so an error raised while committing surfaces here.
        if (typeof __dispatchError__ === 'function')
            __dispatchError__(args[0], args[1], String(e && e.stack ? e.stack : e));
    } finally {
        --__pulpDispatchDepth__;
    }
    if (outermost && typeof __pulpAfterDiscreteDispatch__ === 'function') {
        try { __pulpAfterDiscreteDispatch__(args[0], args[1]); }
        catch (e) {
            if (typeof __dispatchError__ === 'function')
                __dispatchError__(args[0], args[1], String(e && e.stack ? e.stack : e));
        }
    }
    return result;
}
function __dispatchCallbackOnly__(id, eventName) {
    return __pulpRunDispatchBatch__(__pulpDispatchCallbackOnlyUnbatched__, arguments) || 0;
}
function __dispatch__(id, eventName) {
    return __pulpRunDispatchBatch__(__pulpDispatchUnbatched__, arguments);
}
)";

} // namespace

std::string dispatch_prelude_js() {
    return std::string(kDispatchPrelude) + kDispatchBatchPrelude;
}

void BridgeCallbackState::retire_dispatch() noexcept {
    std::lock_guard<std::recursive_mutex> lock(dispatch_mutex_);
    alive.store(false, std::memory_order_release);
    // A callback closure can outlive its bridge. Drop the root so a late
    // dispatch raises no gate on a tree this bridge no longer serves.
    dispatch_root_ = nullptr;
}

BridgeCallbackScope::BridgeCallbackScope(
    const std::shared_ptr<BridgeCallbackState>& state) noexcept
    : state_(state) {
    if (!state_) return;
    if (View* root = state_->dispatch_root())
        lease_.emplace(*root, DispatchLease::Drain::deferred);
}

BridgeCallbackScope::~BridgeCallbackScope() = default;

std::string js_string_literal(std::string_view text) {
    return choc::json::toString(choc::value::createString(std::string(text)), false);
}

void safe_dispatch_eval(const std::shared_ptr<BridgeCallbackState>& alive,
                        ScriptEngine* engine,
                        const std::string& js,
                        const char* context) {
    PULP_TRACE_SCOPE_NAMED("js", "dom_event_dispatch");
    if (!alive || engine == nullptr) return;
    std::lock_guard<std::recursive_mutex> lock(alive->dispatch_mutex());
    if (!alive->load(std::memory_order_acquire) || !alive->engine_alive()) return;
    try {
        if (!static_cast<bool>(*engine)) return;
        {
            PULP_TRACE_SCOPE_NAMED("js", "dom_event_evaluate");
            engine->evaluate(js);
            // A handler that returned with spans still open would nest every
            // later slice under them. Close them here, loudly.
            js_trace::force_close_open_scopes(context);
        }
        // Pump microtasks so React setState commits (and any queueMicrotask /
        // Promise.then continuations scheduled by the handler) before the next
        // event arrives. Without this, drag-style interactions see stale state
        // on the immediately-following pointermove and silently bail.
        {
            PULP_TRACE_SCOPE_NAMED("js", "dom_event_microtask_pump");
            engine->pump_message_loop();
            js_trace::force_close_open_scopes(context);
        }
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge " << context << " error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge " << context << " error: unknown exception\n";
    }
}

void safe_dispatch_eval(ScriptEngine& engine, const std::string& js, const char* context) {
    // The no-flag path targets a known-valid engine reference; delegate through
    // an always-alive flag so both overloads share one implementation.
    static const auto always_alive =
        std::make_shared<BridgeCallbackState>(nullptr);
    safe_dispatch_eval(always_alive, &engine, js, context);
}

void dispatch_event(const std::shared_ptr<BridgeCallbackState>& alive,
                    ScriptEngine* engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr) {
    std::string js = "__dispatch__(" + js_string_literal(id) + ", '" + event_name + "', ";
    js.append(payload_expr.data(), payload_expr.size());
    js += ")";
    safe_dispatch_eval(alive, engine, js, event_name.c_str());
}

void dispatch_event(ScriptEngine& engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr) {
    static const auto always_alive =
        std::make_shared<BridgeCallbackState>(nullptr);
    dispatch_event(always_alive, &engine, id, event_name, payload_expr);
}

void dispatch_callback_only(const std::shared_ptr<BridgeCallbackState>& alive,
                            ScriptEngine* engine,
                            std::string_view id,
                            const std::string& event_name,
                            std::string_view payload_expr) {
    std::string js = "__dispatchCallbackOnly__(" + js_string_literal(id) +
                     ", '" + event_name + "', ";
    js.append(payload_expr.data(), payload_expr.size());
    js += ")";
    safe_dispatch_eval(alive, engine, js, event_name.c_str());
}

} // namespace pulp::view
