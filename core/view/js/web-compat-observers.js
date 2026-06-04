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
    Object.defineProperties(NoOpObserver.prototype, {
        observe: { value: function () {}, configurable: true, writable: true },
        unobserve: { value: function () {}, configurable: true, writable: true },
        disconnect: { value: function () {}, configurable: true, writable: true },
        takeRecords: { value: function () { return []; }, configurable: true, writable: true }
    });

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
        Object.defineProperties(Ctor.prototype, {
            observe: { value: NoOpObserver.prototype.observe, configurable: true, writable: true },
            unobserve: { value: NoOpObserver.prototype.unobserve, configurable: true, writable: true },
            disconnect: { value: NoOpObserver.prototype.disconnect, configurable: true, writable: true },
            takeRecords: { value: NoOpObserver.prototype.takeRecords, configurable: true, writable: true }
        });
        return Ctor;
    }

    defineGlobalIfMissing("MutationObserver", makeObserverCtor());
    defineGlobalIfMissing("IntersectionObserver", makeObserverCtor());
    defineGlobalIfMissing("ResizeObserver", makeObserverCtor());
    defineGlobalIfMissing("PerformanceObserver", makeObserverCtor());

    if (typeof globalThis.AbortController === "undefined" || typeof globalThis.AbortSignal === "undefined") {
        function AbortSignal() {
            this.aborted = false;
            this.reason = undefined;
            this.onabort = null;
            this._listeners = [];
        }

        Object.defineProperties(AbortSignal.prototype, {
            addEventListener: {
                value: function (type, listener) {
                    if (type === "abort" && typeof listener === "function") {
                        this._listeners.push(listener);
                    }
                },
                configurable: true,
                writable: true
            },
            removeEventListener: {
                value: function (type, listener) {
                    if (type !== "abort") return;
                    this._listeners = this._listeners.filter(function (fn) { return fn !== listener; });
                },
                configurable: true,
                writable: true
            },
            dispatchEvent: {
                value: function (event) {
                    var type = event && event.type ? event.type : "";
                    if (type !== "abort") return true;
                    var listeners = this._listeners.slice();
                    for (var i = 0; i < listeners.length; ++i) {
                        listeners[i].call(this, event);
                    }
                    if (typeof this.onabort === "function") {
                        this.onabort.call(this, event);
                    }
                    return true;
                },
                configurable: true,
                writable: true
            },
            throwIfAborted: {
                value: function () {
                    if (this.aborted) {
                        throw (this.reason || new Error("The operation was aborted"));
                    }
                },
                configurable: true,
                writable: true
            }
        });

        AbortSignal.abort = function (reason) {
            var signal = new AbortSignal();
            signal.aborted = true;
            signal.reason = reason || new Error("The operation was aborted");
            return signal;
        };
        AbortSignal.any = function (signals) {
            var controller = new AbortController();
            for (var i = 0; signals && i < signals.length; ++i) {
                var signal = signals[i];
                if (!signal) continue;
                if (signal.aborted) {
                    controller.abort(signal.reason);
                    break;
                }
                signal.addEventListener("abort", function (event) {
                    controller.abort(event && event.target ? event.target.reason : undefined);
                });
            }
            return controller.signal;
        };

        function AbortController() {
            this.signal = new AbortSignal();
        }

        Object.defineProperties(AbortController.prototype, {
            abort: {
                value: function (reason) {
                    if (this.signal.aborted) return;
                    this.signal.aborted = true;
                    this.signal.reason = reason || new Error("The operation was aborted");
                    this.signal.dispatchEvent({ type: "abort", target: this.signal });
                },
                configurable: true,
                writable: true
            }
        });

        defineGlobalIfMissing("AbortSignal", AbortSignal);
        defineGlobalIfMissing("AbortController", AbortController);
    }

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
        Object.defineProperties(XMLHttpRequest.prototype, {
            open: { value: function () {}, configurable: true, writable: true },
            send: { value: function () {}, configurable: true, writable: true },
            abort: { value: function () {}, configurable: true, writable: true },
            setRequestHeader: { value: function () {}, configurable: true, writable: true },
            getResponseHeader: { value: function () { return null; }, configurable: true, writable: true },
            getAllResponseHeaders: { value: function () { return ""; }, configurable: true, writable: true },
            addEventListener: { value: function () {}, configurable: true, writable: true },
            removeEventListener: { value: function () {}, configurable: true, writable: true }
        });
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
            Object.defineProperty(Element.prototype, "scrollTo", {
                value: function () {},
                configurable: true,
                writable: true
            });
        }
        if (typeof Element.prototype.scrollIntoView !== "function") {
            Object.defineProperty(Element.prototype, "scrollIntoView", {
                value: function () {},
                configurable: true,
                writable: true
            });
        }
    }
})();
