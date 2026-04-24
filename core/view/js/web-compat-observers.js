// web-compat-observers.js — No-op browser observer + dev-mode stubs.
//
// React 18 dev (and many other framework dev builds) feature-detect a small
// cohort of browser APIs with `typeof X === 'function'`. They never *require*
// the callbacks to fire — they only need the constructor to exist so the
// detection branch chooses the polyfilled fast-path. Returning a no-op class
// keeps us spec-shaped without paying the engineering cost of implementing
// real Intersection / Mutation / Resize / Performance reporting against our
// virtual DOM.
//
// See pulp #468 (gap matrix) for the full list and the per-API call counts
// against react-dom.development.js.

(function () {
    function NoOpObserver() {
        // Constructor accepts (callback) per spec; we ignore it because we
        // never actually trigger observation events.
    }
    NoOpObserver.prototype.observe = function () {};
    NoOpObserver.prototype.unobserve = function () {};
    NoOpObserver.prototype.disconnect = function () {};
    NoOpObserver.prototype.takeRecords = function () { return []; };

    function defineGlobalIfMissing(name, value) {
        if (typeof globalThis[name] === "undefined") {
            globalThis[name] = value;
        }
    }

    // Each cohort member gets its own constructor function so
    // `typeof MutationObserver === 'function'` is true and `instanceof`
    // checks don't cross-pollute.
    function makeObserverCtor() {
        function Ctor(cb) { this._cb = cb; }
        Ctor.prototype.observe = NoOpObserver.prototype.observe;
        Ctor.prototype.unobserve = NoOpObserver.prototype.unobserve;
        Ctor.prototype.disconnect = NoOpObserver.prototype.disconnect;
        Ctor.prototype.takeRecords = NoOpObserver.prototype.takeRecords;
        return Ctor;
    }

    defineGlobalIfMissing("MutationObserver", makeObserverCtor());
    defineGlobalIfMissing("IntersectionObserver", makeObserverCtor());
    defineGlobalIfMissing("ResizeObserver", makeObserverCtor());
    defineGlobalIfMissing("PerformanceObserver", makeObserverCtor());

    // ── XMLHttpRequest stub ──────────────────────────────────────────────
    // React dev mode reads `typeof XMLHttpRequest === 'function'` while
    // building error stacks (it tries to look up the source file via
    // synchronous XHR). Returning a no-op class keeps the typeof check
    // happy; React falls back to its inline source-map path.
    if (typeof globalThis.XMLHttpRequest === "undefined") {
        function XMLHttpRequest() {
            this.readyState = 0;
            this.status = 0;
            this.responseText = "";
            this.response = null;
        }
        XMLHttpRequest.UNSENT = 0;
        XMLHttpRequest.OPENED = 1;
        XMLHttpRequest.HEADERS_RECEIVED = 2;
        XMLHttpRequest.LOADING = 3;
        XMLHttpRequest.DONE = 4;
        XMLHttpRequest.prototype.open = function () {};
        XMLHttpRequest.prototype.send = function () {};
        XMLHttpRequest.prototype.abort = function () {};
        XMLHttpRequest.prototype.setRequestHeader = function () {};
        XMLHttpRequest.prototype.getResponseHeader = function () { return null; };
        XMLHttpRequest.prototype.getAllResponseHeaders = function () { return ""; };
        XMLHttpRequest.prototype.addEventListener = function () {};
        XMLHttpRequest.prototype.removeEventListener = function () {};
        globalThis.XMLHttpRequest = XMLHttpRequest;
    }

    // ── Element.prototype.scrollTop / scrollLeft ─────────────────────────
    // React dev warnings query scroll positions when reporting input
    // focus issues. Always returning 0 is safe — we don't model scroll.
    if (typeof Element !== "undefined" && Element.prototype) {
        if (!Object.getOwnPropertyDescriptor(Element.prototype, "scrollTop")) {
            Object.defineProperty(Element.prototype, "scrollTop", {
                get: function () { return 0; },
                set: function () {},
                configurable: true
            });
        }
        if (!Object.getOwnPropertyDescriptor(Element.prototype, "scrollLeft")) {
            Object.defineProperty(Element.prototype, "scrollLeft", {
                get: function () { return 0; },
                set: function () {},
                configurable: true
            });
        }
        if (!Object.getOwnPropertyDescriptor(Element.prototype, "scrollWidth")) {
            Object.defineProperty(Element.prototype, "scrollWidth", {
                get: function () { return this.clientWidth || 0; },
                configurable: true
            });
        }
        if (!Object.getOwnPropertyDescriptor(Element.prototype, "scrollHeight")) {
            Object.defineProperty(Element.prototype, "scrollHeight", {
                get: function () { return this.clientHeight || 0; },
                configurable: true
            });
        }
        if (typeof Element.prototype.scrollTo !== "function") {
            Element.prototype.scrollTo = function () {};
        }
        if (typeof Element.prototype.scrollIntoView !== "function") {
            Element.prototype.scrollIntoView = function () {};
        }
    }
})();
