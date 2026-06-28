// web-compat-animation.js - browser-shaped Element.animate() shim.
//
// Embed order: loaded after Element, event helpers, and CSSStyleDeclaration
// reflection. The shim resolves requestAnimationFrame/cancelAnimationFrame at
// play time so scheduler installation can remain later in the prelude chain.

(function() {
    if (typeof Element !== "function") return;

    function raf(fn) {
        if (typeof requestAnimationFrame === "function") return requestAnimationFrame(fn);
        if (typeof window !== "undefined" && typeof window.requestAnimationFrame === "function")
            return window.requestAnimationFrame(fn);
        return 0;
    }

    function cancelRaf(id) {
        if (!id) return;
        if (typeof cancelAnimationFrame === "function") cancelAnimationFrame(id);
        else if (typeof window !== "undefined" && typeof window.cancelAnimationFrame === "function")
            window.cancelAnimationFrame(id);
    }

    function nowMs() {
        if (typeof performance !== "undefined" && performance && typeof performance.now === "function")
            return performance.now();
        if (typeof Date !== "undefined" && Date.now) return Date.now();
        return 0;
    }

    function normalizeKeyframes(keyframes) {
        if (Array.isArray(keyframes)) return keyframes.length ? keyframes : [{}];
        var first = {};
        var last = {};
        keyframes = keyframes || {};
        for (var prop in keyframes) {
            if (prop === "offset" || prop === "easing" || prop === "composite") continue;
            var value = keyframes[prop];
            if (Array.isArray(value)) {
                first[prop] = value.length ? value[0] : "";
                last[prop] = value.length ? value[value.length - 1] : "";
            } else {
                last[prop] = value;
            }
        }
        return [first, last];
    }

    function numericPart(value) {
        var n = parseFloat(value);
        return isNaN(n) ? null : n;
    }

    function unitPart(value) {
        var unit = String(value).replace(/^[+-]?(?:\d+|\d*\.\d+)(?:e[+-]?\d+)?/i, "");
        return unit || "";
    }

    function easingValue(name, t) {
        if (name === "ease-in") return t * t * t;
        if (name === "ease-out") return 1 - Math.pow(1 - t, 3);
        if (name === "ease-in-out") return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
        return t;
    }

    function Animation(el, keyframes, options) {
        options = (typeof options === "number") ? { duration: options } : (options || {});
        this._el = el;
        this._keyframes = normalizeKeyframes(keyframes);
        this._duration = options.duration !== undefined ? Math.max(0, Number(options.duration) || 0) : 300;
        this._easing = options.easing || "linear";
        this._fill = options.fill || "none";
        this._startTime = null;
        this._elapsedBeforePause = 0;
        this._startProps = null;
        this._rafId = 0;
        this._finished = false;
        this._cancelled = false;
        this._finishCallbacks = [];
        this._cancelCallbacks = [];
        this.onfinish = null;
        this.oncancel = null;
        this.playState = "idle";

        var self = this;
        this.finished = {
            then: function(fn, onRejected) {
                if (typeof fn === "function") {
                    if (self._finished) fn(self);
                    else if (!self._cancelled) self._finishCallbacks.push(fn);
                }
                if (typeof onRejected === "function") {
                    if (self._cancelled) onRejected(self);
                    else if (!self._finished) self._cancelCallbacks.push(onRejected);
                }
                return this;
            }
        };
    }

    Animation.prototype._captureStartProps = function() {
        if (this._startProps) return;
        var first = this._keyframes[0] || {};
        var last = this._keyframes[this._keyframes.length - 1] || {};
        this._startProps = {};
        for (var prop in last) {
            if (prop === "offset" || prop === "easing" || prop === "composite") continue;
            this._startProps[prop] = this._el.style[prop] || first[prop] || "";
        }
    };

    Animation.prototype._applyAt = function(progress) {
        this._captureStartProps();
        var eased = easingValue(this._easing, Math.max(0, Math.min(progress, 1)));
        var first = this._keyframes[0] || {};
        var last = this._keyframes[this._keyframes.length - 1] || {};
        for (var prop in last) {
            if (prop === "offset" || prop === "easing" || prop === "composite") continue;
            var fromVal = first[prop] !== undefined ? first[prop] : this._startProps[prop];
            var toVal = last[prop];
            var fromNum = numericPart(fromVal);
            var toNum = numericPart(toVal);
            if (fromNum !== null && toNum !== null) {
                var unit = unitPart(toVal) || unitPart(fromVal);
                this._el.style[prop] = String(fromNum + (toNum - fromNum) * eased) + unit;
            } else if (progress >= 1) {
                this._el.style[prop] = toVal;
            } else if (progress <= 0 && fromVal !== undefined) {
                this._el.style[prop] = fromVal;
            }
        }
    };

    Animation.prototype._complete = function() {
        if (this._finished || this._cancelled) return;
        cancelRaf(this._rafId);
        this._rafId = 0;
        this._finished = true;
        this.playState = "finished";
        if (this._fill === "none" && this._startProps) {
            for (var prop in this._startProps) this._el.style[prop] = this._startProps[prop];
        }
        if (typeof this.onfinish === "function") this.onfinish(this);
        var callbacks = this._finishCallbacks.slice();
        this._finishCallbacks.length = 0;
        for (var i = 0; i < callbacks.length; i++) callbacks[i](this);
        if (typeof _makeEvent === "function") this._el.dispatchEvent(_makeEvent("animationend", this._el));
        else this._el.dispatchEvent(new Event("animationend", { bubbles: true, cancelable: false }));
    };

    Animation.prototype._schedule = function() {
        var self = this;
        this._rafId = raf(function() {
            if (self._cancelled || self.playState !== "running") return;
            var elapsed = self._duration <= 0 ? self._duration : nowMs() - self._startTime;
            var progress = self._duration <= 0 ? 1 : Math.min(elapsed / self._duration, 1);
            self._applyAt(progress);
            if (progress >= 1) self._complete();
            else self._schedule();
        });
    };

    Animation.prototype.play = function() {
        if (this._cancelled) return;
        if (this.playState === "running") return;
        var wasPaused = this.playState === "paused";
        if (this._finished) {
            this._finished = false;
            this._elapsedBeforePause = 0;
            this._startTime = null;
            wasPaused = false;
        }
        this.playState = "running";
        this._captureStartProps();
        if (this._duration <= 0) {
            this._applyAt(1);
            this._complete();
            return;
        }
        if (!wasPaused) {
            this._elapsedBeforePause = 0;
            this._applyAt(0);
        }
        this._startTime = nowMs() - this._elapsedBeforePause;
        this._schedule();
    };

    Animation.prototype.pause = function() {
        if (this.playState !== "running") return;
        this._elapsedBeforePause = Math.max(0, nowMs() - this._startTime);
        cancelRaf(this._rafId);
        this._rafId = 0;
        this.playState = "paused";
    };

    Animation.prototype.cancel = function() {
        cancelRaf(this._rafId);
        this._rafId = 0;
        if (this._startProps) {
            for (var prop in this._startProps) this._el.style[prop] = this._startProps[prop];
        }
        this._cancelled = true;
        this._finished = false;
        this.playState = "idle";
        if (typeof this.oncancel === "function") this.oncancel(this);
        var callbacks = this._cancelCallbacks.slice();
        this._cancelCallbacks.length = 0;
        for (var i = 0; i < callbacks.length; i++) callbacks[i](this);
    };

    Animation.prototype.finish = function() {
        if (this._cancelled) return;
        this._applyAt(1);
        this._complete();
    };

    Element.prototype.animate = function(keyframes, options) {
        var anim = new Animation(this, keyframes, options);
        anim.play();
        return anim;
    };

    if (typeof globalThis !== "undefined" && typeof globalThis.Animation === "undefined")
        globalThis.Animation = Animation;
})();
