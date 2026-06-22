// pulp-react-dom-shim.mjs
//
// Drop-in replacement for `react-dom/client` (and `react-dom`) that
// reroutes through `@pulp/react`'s reconciler.
//
// Why: bundled React-DOM 18 attaches delegated root listeners to the
// container and dispatches events through its plugin event system.
// Pulp's web-compat DOM shim doesn't satisfy enough of the
// EventTarget/Event spec for React-DOM's plugin system to actually
// fire JSX handlers (confirmed via byte-equal PNG probes after
// simulate_drag + Event-instance fix). @pulp/react bypasses that
// entirely: its host-config wires `onClick`/`onMouseDown`/etc. to
// `bridge.on(id, 'click', fn)` DIRECTLY at commit time — react-konva /
// react-three-fiber / React Native Fabric all use this pattern.
//
// This shim's surface mimics react-dom@18.3.1's `client` export:
//   - createRoot(container)
//   - hydrateRoot(container, element)
//
// The bundled `ReactDOMClient.createRoot(mountEl).render(<App/>)` call
// in the jsx-transform entry ends up calling @pulp/react's render.
//
import { createRoot as pulpCreateRoot, render as pulpRender, unmount as pulpUnmount } from '@pulp/react';

// Append-only diagnostic ring. Main process reads this after settle.
function __log(msg) {
    if (typeof globalThis.__pulpShimLog__ !== 'string') globalThis.__pulpShimLog__ = '';
    globalThis.__pulpShimLog__ += msg + '\n';
}
globalThis.__pulpShimLogFn__ = __log;

__log('[shim] module loaded; pulpCreateRoot=' + typeof pulpCreateRoot + ' pulpRender=' + typeof pulpRender);

// Hook window.addEventListener / removeEventListener at shim-load time so we
// can audit every install/remove call. Custom-Fader-style JSX (Chainer's knobs use
// `window.addEventListener('mousemove', ...)` from a useEffect to track
// drag) needs the listener to STICK across React renders. Diagnostic
// showed mouseup:15 but mousemove:0 — either the install never fired
// or removeEventListener succeeded after a re-render but the new
// install didn't. This hook records exact sequence.
(function () {
    var w = (typeof globalThis !== 'undefined') ? globalThis.window : undefined;
    if (!w || typeof w.addEventListener !== 'function') return;
    if (w.__pulpAddELHooked__) return;
    w.__pulpAddELHooked__ = true;
    var origAdd = w.addEventListener.bind(w);
    var origRemove = (typeof w.removeEventListener === 'function') ? w.removeEventListener.bind(w) : null;
    globalThis.__pulpAddELLog__ = [];
    w.addEventListener = function (type, fn, opts) {
        try {
            globalThis.__pulpAddELLog__.push({ op: 'add', type: type, fn: fn ? (fn.name || 'anon') : 'null', stack: (new Error()).stack.split('\n').slice(1, 4).join('|') });
        } catch (e) {}
        return origAdd(type, fn, opts);
    };
    if (origRemove) {
        w.removeEventListener = function (type, fn, opts) {
            try {
                globalThis.__pulpAddELLog__.push({ op: 'remove', type: type, fn: fn ? (fn.name || 'anon') : 'null' });
            } catch (e) {}
            return origRemove(type, fn, opts);
        };
    }
})();

class PulpReactRoot {
    constructor(container) {
        // The bundle calls ReactDOMClient.createRoot(mountEl) where
        // mountEl is document.getElementById('root') || document.body.
        // document.body is the JS Element with _id='__root__' — but the
        // C++ widgets_ map does NOT contain '__root__' (it's the JS-side
        // body sentinel, not a bridge widget). The convention in
        // @pulp/react is rootId='' → resolves to the bridge's implicit
        // root_ View via widget_bridge.cpp's resolve_parent fallback.
        // Always use '' so widgets attach to root_ correctly.
        this._container = pulpCreateRoot('');
        this._lastElement = null;
    }
    render(element) {
        this._lastElement = element;
        try {
            // Diagnostic: confirm we got here + report what pulpRender does.
            __log('[shim] PulpReactRoot.render called; element=' + (element && typeof element === 'object' ? (element.type && element.type.name ? element.type.name : typeof element.type) : typeof element));
            const r = pulpRender(element, this._container);
            __log('[shim] pulpRender returned ok');
            return r;
        } catch (e) {
            // Surface the error via global so probe can read it.
            globalThis.__pulpShimError__ = String(e && e.stack ? e.stack.slice(0, 500) : e);
            __log('[shim] ERROR: ' + globalThis.__pulpShimError__);
            throw e;
        }
    }
    unmount() {
        pulpUnmount(this._container);
        this._lastElement = null;
    }
}

// react-dom@18.3.1 `client` API
export function createRoot(container, _options) {
    return new PulpReactRoot(container);
}

export function hydrateRoot(container, element, _options) {
    const root = new PulpReactRoot(container);
    if (element !== undefined) root.render(element);
    return root;
}

// Default export shape for `import ReactDOM from 'react-dom'`
export default { createRoot, hydrateRoot };

// Legacy `ReactDOM.render(<App/>, container)` for older user code.
export function render(element, container) {
    const root = createRoot(container);
    root.render(element);
    return root;
}

// Legacy `ReactDOM.unmountComponentAtNode(container)`.
export function unmountComponentAtNode(container) {
    // Best-effort. The legacy API didn't return a root, so we can't
    // reliably look up the same root the caller created. Plugin
    // authors using new ReactDOM should call root.unmount() directly.
    return false;
}

// react-dom/client also re-exports findDOMNode in some versions.
export function findDOMNode(component) {
    return component && component._dom ? component._dom : null;
}

// flushSync passthrough — most user code expects this to exist even
// if the reconciler runs synchronously by default in @pulp/react.
export function flushSync(fn) {
    return typeof fn === 'function' ? fn() : undefined;
}
