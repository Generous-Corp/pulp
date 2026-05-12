// web-compat-scheduler.js — Microtask + timers + animation-frame +
// MessageChannel + URLSearchParams native registration.
//
// React 18's concurrent-mode scheduler (`scheduler/src/forks/Scheduler.js`)
// prefers `MessageChannel` for cross-task fan-out so it can yield back to
// the host between renders. When MessageChannel is missing it falls back to
// `setTimeout(0)` — still functional, just less granular. For the import-
// time first-commit harvest (pulp #468) we don't need real concurrency,
// only the constructor plus port wiring so React's feature-detect resolves
// to its preferred path.
//
// pulp #915 — animation-frame and timer globals are now registered here
// (driven by the underscored `__requestFrame__` / `__scheduleTimer__`
// natives that WidgetBridge::register_api installs) so consumer bundles
// don't need a JS shim that re-aliases `__requestFrame__` ⇄
// `requestAnimationFrame`. The deletion of spectr's ~80-line shim.js
// hangs off this slot.
//
// This shim is intentionally thin: the message queue is drained on the
// next microtask via `Promise.resolve().then`. That keeps the scheduling
// order deterministic for synchronous test harnesses and avoids leaking a
// timer-driven event loop that the QuickJS embedding would never pump.

(function () {
    // ── queueMicrotask ───────────────────────────────────────────────────
    if (typeof globalThis.queueMicrotask !== "function") {
        globalThis.queueMicrotask = function (fn) {
            if (typeof fn !== "function") return;
            // Prefer Promise (true microtask). Fall back to setTimeout if
            // Promise is somehow missing (shouldn't be in QuickJS / JSC / V8).
            if (typeof Promise !== "undefined") {
                Promise.resolve().then(function () {
                    try { fn(); }
                    catch (e) {
                        if (typeof console !== "undefined" && console.error) {
                            console.error("queueMicrotask:", e);
                        }
                    }
                });
            } else if (typeof setTimeout === "function") {
                setTimeout(fn, 0);
            }
        };
    }

    // ── requestAnimationFrame / cancelAnimationFrame (pulp #915) ─────────
    // Wraps the native __requestFrame__ id-allocator + __frameCallbacks__
    // table that WidgetBridge installs in its preamble. Standard names go
    // on globalThis directly so consumers don't need a shim.
    if (typeof globalThis.requestAnimationFrame !== "function"
            && typeof __requestFrame__ === "function") {
        globalThis.requestAnimationFrame = function (fn) {
            if (typeof fn !== "function") return 0;
            var id = __frameNextId__++;
            __frameCallbacks__[id] = fn;
            return __requestFrame__(id);
        };
        globalThis.cancelAnimationFrame = function (id) {
            if (typeof id !== "number") return;
            delete __frameCallbacks__[id];
            if (typeof __cancelFrame__ === "function") __cancelFrame__(id);
        };
    }

    // ── setTimeout / clearTimeout / setInterval / clearInterval (#915) ───
    // Driven by the native deadline tracker (__scheduleTimer__) so the
    // frame loop fires positive-delay timers without the consumer having
    // to chain rAF ticks. setTimeout(fn, 0) bypasses native scheduling
    // entirely and routes through the microtask queue, so it fires
    // deterministically on the next pump_message_loop() — which is the
    // contract React's scheduler relies on for its yield path.
    if (typeof globalThis.setTimeout !== "function"
            && typeof __scheduleTimer__ === "function") {
        function __make_timer(fn, ms, repeat) {
            if (typeof fn !== "function") return 0;
            var id = __timerNextId__++;
            __timerCallbacks__[id] = { fn: fn, repeat: !!repeat };
            var delay = (typeof ms === "number" && ms > 0) ? ms : 0;
            if (delay <= 0 && !repeat) {
                // Fast-path setTimeout(fn, 0): drain on the next microtask
                // so a single pump_message_loop() call fires it. Still
                // honours clearTimeout — the wrapper checks the registry.
                Promise.resolve().then(function () {
                    if (__timerCallbacks__[id]) __invokeTimer__(id);
                });
                return id;
            }
            __scheduleTimer__(id, delay, !!repeat);
            return id;
        }
        globalThis.setTimeout = function (fn, ms) {
            return __make_timer(fn, ms, false);
        };
        globalThis.setInterval = function (fn, ms) {
            // Spec: setInterval clamps to a 4ms minimum, which avoids a
            // native flush firing the same interval every pump.
            var clamped = (typeof ms === "number" && ms >= 4) ? ms : 4;
            return __make_timer(fn, clamped, true);
        };
        globalThis.clearTimeout = function (id) {
            if (typeof id !== "number") return;
            delete __timerCallbacks__[id];
            if (typeof __cancelTimer__ === "function") __cancelTimer__(id);
        };
        globalThis.clearInterval = globalThis.clearTimeout;
    }

    // ── performance.now() (pulp #915) ────────────────────────────────────
    // Bundled-React frameworks read `performance.now` at module-eval
    // time; without it the bundle ReferenceErrors before the bridge has
    // a chance to install the legacy window.performance shim.
    if (typeof globalThis.performance === "undefined"
            && typeof __performanceNow__ === "function") {
        globalThis.performance = {
            now: function () { return __performanceNow__(); }
        };
    }

    // ── MessageChannel + MessagePort + postMessage ───────────────────────
    if (typeof globalThis.MessageChannel === "undefined") {
        function MessagePort() {
            this._otherPort = null;
            this._listeners = [];
            this._onmessage = null;
            this._started = false;
            this._queue = [];
        }
        MessagePort.prototype.postMessage = function (data) {
            var other = this._otherPort;
            if (!other) return;
            // Always defer delivery to a microtask so MessageChannel
            // semantics match the platform: fire-and-forget, never sync.
            globalThis.queueMicrotask(function () {
                var event = { data: data, source: null, ports: [] };
                if (other._onmessage) {
                    try { other._onmessage(event); } catch (e) {
                        if (typeof console !== "undefined" && console.error) {
                            console.error("MessagePort.onmessage:", e);
                        }
                    }
                }
                for (var i = 0; i < other._listeners.length; i++) {
                    try { other._listeners[i](event); } catch (e2) {
                        if (typeof console !== "undefined" && console.error) {
                            console.error("MessagePort listener:", e2);
                        }
                    }
                }
            });
        };
        MessagePort.prototype.addEventListener = function (type, fn) {
            if (type !== "message" || typeof fn !== "function") return;
            this._listeners.push(fn);
        };
        MessagePort.prototype.removeEventListener = function (type, fn) {
            if (type !== "message") return;
            var idx = this._listeners.indexOf(fn);
            if (idx >= 0) this._listeners.splice(idx, 1);
        };
        MessagePort.prototype.start = function () { this._started = true; };
        MessagePort.prototype.close = function () {
            this._listeners.length = 0;
            this._onmessage = null;
            this._otherPort = null;
        };
        Object.defineProperty(MessagePort.prototype, "onmessage", {
            get: function () { return this._onmessage; },
            set: function (fn) { this._onmessage = fn; this._started = true; },
            configurable: true
        });

        function MessageChannel() {
            this.port1 = new MessagePort();
            this.port2 = new MessagePort();
            this.port1._otherPort = this.port2;
            this.port2._otherPort = this.port1;
        }

        globalThis.MessagePort = MessagePort;
        globalThis.MessageChannel = MessageChannel;
    }

    // ── window.postMessage (no-op so feature-detects pass) ───────────────
    // Real cross-frame postMessage doesn't apply in our JS engine, but
    // some libs read `typeof window.postMessage === 'function'` to choose
    // between MessageChannel and window-bound paths. React 18's scheduler
    // checks `window.postMessage` specifically — and in this runtime
    // `window` is a distinct object created in web-compat-document.js, so
    // assigning on globalThis alone doesn't cover the check. Mirror onto
    // both. (pulp #468 Codex P2.)
    if (typeof globalThis.postMessage !== "function") {
        globalThis.postMessage = function () {};
    }
    if (typeof globalThis.window === "object" && globalThis.window !== null
            && typeof globalThis.window.postMessage !== "function") {
        globalThis.window.postMessage = globalThis.postMessage;
    }

    // ── window.parent (self-reference) ───────────────────────────────────
    // Real browser frames give nested windows a `parent` that points at the
    // embedding frame; standalone top-level windows have `window.parent ===
    // window`. Imported React designs commonly call
    // `window.parent.postMessage(...)` for cross-frame messaging (4 sites
    // in Spectr's edit-mode bridge alone); without a self-reference, the
    // property access throws TypeError and the calling effect is silently
    // dead. We're always a top-level window in this runtime, so the
    // self-reference is spec-correct.
    if (typeof globalThis.window === "object" && globalThis.window !== null
            && typeof globalThis.window.parent === "undefined") {
        globalThis.window.parent = globalThis.window;
    }

    // ── URLSearchParams ──────────────────────────────────────────────────
    // Compact, spec-shaped polyfill — covers React's usages (read-only
    // parse + iteration) and the common app-side construct/append/get
    // pattern. Doesn't try to faithfully implement every edge of the
    // WHATWG spec (e.g. plus-decoding subtleties) — sufficient for the
    // import-time DOM harvest.
    if (typeof globalThis.URLSearchParams === "undefined") {
        function decode(s) {
            try { return decodeURIComponent(String(s).replace(/\+/g, " ")); }
            catch (e) { return String(s); }
        }
        function encode(s) {
            return encodeURIComponent(String(s)).replace(/%20/g, "+");
        }
        function URLSearchParams(init) {
            this._pairs = [];
            if (init == null) return;
            if (init instanceof URLSearchParams) {
                for (var i = 0; i < init._pairs.length; i++) {
                    this._pairs.push([init._pairs[i][0], init._pairs[i][1]]);
                }
                return;
            }
            if (typeof init === "string") {
                var src = init;
                if (src.charAt(0) === "?") src = src.slice(1);
                if (!src) return;
                var parts = src.split("&");
                for (var p = 0; p < parts.length; p++) {
                    if (!parts[p]) continue;
                    var eq = parts[p].indexOf("=");
                    if (eq < 0) {
                        this._pairs.push([decode(parts[p]), ""]);
                    } else {
                        this._pairs.push([decode(parts[p].slice(0, eq)),
                                          decode(parts[p].slice(eq + 1))]);
                    }
                }
                return;
            }
            // Sequence-of-pairs / record init (plain object).
            if (typeof init === "object") {
                if (typeof init.length === "number") {
                    for (var k = 0; k < init.length; k++) {
                        var pair = init[k];
                        if (pair && pair.length >= 2) {
                            this._pairs.push([String(pair[0]), String(pair[1])]);
                        }
                    }
                } else {
                    for (var key in init) {
                        if (Object.prototype.hasOwnProperty.call(init, key)) {
                            this._pairs.push([String(key), String(init[key])]);
                        }
                    }
                }
            }
        }
        URLSearchParams.prototype.append = function (name, value) {
            this._pairs.push([String(name), String(value)]);
        };
        URLSearchParams.prototype["delete"] = function (name) {
            for (var i = this._pairs.length - 1; i >= 0; i--) {
                if (this._pairs[i][0] === String(name)) this._pairs.splice(i, 1);
            }
        };
        URLSearchParams.prototype.get = function (name) {
            for (var i = 0; i < this._pairs.length; i++) {
                if (this._pairs[i][0] === String(name)) return this._pairs[i][1];
            }
            return null;
        };
        URLSearchParams.prototype.getAll = function (name) {
            var out = [];
            for (var i = 0; i < this._pairs.length; i++) {
                if (this._pairs[i][0] === String(name)) out.push(this._pairs[i][1]);
            }
            return out;
        };
        URLSearchParams.prototype.has = function (name) {
            for (var i = 0; i < this._pairs.length; i++) {
                if (this._pairs[i][0] === String(name)) return true;
            }
            return false;
        };
        URLSearchParams.prototype.set = function (name, value) {
            // Spec: if the name exists, the first match keeps its position
            // and gets the new value; later duplicates are removed. If it
            // doesn't exist, append.
            var key = String(name);
            var firstIndex = -1;
            for (var i = 0; i < this._pairs.length; i++) {
                if (this._pairs[i][0] === key) { firstIndex = i; break; }
            }
            if (firstIndex < 0) {
                this._pairs.push([key, String(value)]);
                return;
            }
            this._pairs[firstIndex][1] = String(value);
            for (var j = this._pairs.length - 1; j > firstIndex; j--) {
                if (this._pairs[j][0] === key) this._pairs.splice(j, 1);
            }
        };
        URLSearchParams.prototype.toString = function () {
            var out = "";
            for (var i = 0; i < this._pairs.length; i++) {
                if (i > 0) out += "&";
                out += encode(this._pairs[i][0]) + "=" + encode(this._pairs[i][1]);
            }
            return out;
        };
        URLSearchParams.prototype.forEach = function (fn, thisArg) {
            for (var i = 0; i < this._pairs.length; i++) {
                fn.call(thisArg, this._pairs[i][1], this._pairs[i][0], this);
            }
        };

        globalThis.URLSearchParams = URLSearchParams;
    }

    // ── Mirror standard names onto `window` (pulp #915) ──────────────────
    // web-compat-document.js installs a `window` object before this file
    // runs and pre-populates it with its own requestAnimationFrame /
    // cancelAnimationFrame closures. React's scheduler reads
    // `window.setTimeout` / `window.requestAnimationFrame` specifically
    // (not just globalThis), so collapse the two onto a single shared
    // implementation now that the natively-driven ones exist.
    if (typeof globalThis.window === "object" && globalThis.window !== null) {
        var w = globalThis.window;
        var mirror = ["requestAnimationFrame", "cancelAnimationFrame",
                      "setTimeout", "clearTimeout",
                      "setInterval", "clearInterval",
                      "queueMicrotask", "performance",
                      "MessageChannel", "MessagePort"];
        for (var i = 0; i < mirror.length; i++) {
            var n = mirror[i];
            if (typeof globalThis[n] !== "undefined") w[n] = globalThis[n];
        }
    }
})();
