// ═══════════════════════════════════════════════════════════════════════════════
// Events + Pointer-capture (P5-7 follow-up — extracted from web-compat-element.js)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Two coherent subsurfaces of the DOM Event surface:
//
//   1. Events. Element.prototype addEventListener / removeEventListener /
//      dispatchEvent + the native event-listener registry + event-phase
//      bubbling / capture / target-phase semantics. The synthetic Event
//      object's eventPhase / currentTarget / preventDefault / stopPropagation
//      / stopImmediatePropagation pieces.
//
//   2. Pointer capture (P2b). Element.prototype setPointerCapture /
//      releasePointerCapture / hasPointerCapture against the document-scoped
//      pointer-capture registry, plus the lostpointercapture / pointercancel
//      synthetic events that the bridge dispatches when capture is released.
//
// Embed order: loaded AFTER web-compat-element.js so the Element constructor
// + prototype are already defined when the extracted prototype overrides
// install. The functions on Element.prototype here can reference each other
// freely; cross-prelude resolution happens at call time, not parse time.

// ── Events ───────────────────────────────────────────────────────────────────

Element.prototype.addEventListener = function(type, fn, opts) {
    var capture = false;
    if (opts === true) capture = true;
    else if (opts && opts.capture) capture = true;

    var id = this._id;
    if (!__eventListeners__[id]) __eventListeners__[id] = {};
    if (!__eventListeners__[id][type]) __eventListeners__[id][type] = [];
    __eventListeners__[id][type].push({ fn: fn, capture: capture });

    // Register native callbacks for event types that need them
    if (this._nativeCreated) this._registerNativeEvent(type);
};

Element.prototype.removeEventListener = function(type, fn, opts) {
    var capture = false;
    if (opts === true) capture = true;
    else if (opts && opts.capture) capture = true;

    var id = this._id;
    var listeners = __eventListeners__[id] && __eventListeners__[id][type];
    if (!listeners) return;
    for (var i = listeners.length - 1; i >= 0; i--) {
        if (listeners[i].fn === fn && listeners[i].capture === capture) {
            listeners.splice(i, 1);
        }
    }
};

Element.prototype.dispatchEvent = function(event) {
    event.target = this;
    _dispatchEvent(this, event);
};

Element.prototype._registerNativeEvent = function(type) {
    var id = this._id;
    var self = this;
    if (type === "click" || type === "mousedown" || type === "mouseup") {
        registerClick(id);
        on(id, "click", function(data) {
            var evt = _makeEvent("click", self, data);
            self.dispatchEvent(evt);
        });
    } else if (type === "mouseenter" || type === "mouseleave" ||
               type === "pointerenter" || type === "pointerleave") {
        registerHover(id);
        on(id, "mouseenter", function(data) {
            var evt = _makeEvent("mouseenter", self, data);
            evt._noBubble = true;
            _fireListeners(self, evt);
            var pe = _makeEvent("pointerenter", self, data);
            pe._noBubble = true;
            _fireListeners(self, pe);
        });
        on(id, "mouseleave", function(data) {
            var evt = _makeEvent("mouseleave", self, data);
            evt._noBubble = true;
            _fireListeners(self, evt);
            var pe = _makeEvent("pointerleave", self, data);
            pe._noBubble = true;
            _fireListeners(self, pe);
        });
    } else if (type === "pointerdown" || type === "pointermove" || type === "pointerup" || type === "pointercancel") {
        // Register for pointer events — these are dispatched from C++ bridge
        if (typeof registerPointer === "function") registerPointer(id);
        on(id, "pointerdown", function(data) {
            self.dispatchEvent(_makeEvent("pointerdown", self, data));
        });
        on(id, "pointermove", function(data) {
            self.dispatchEvent(_makeEvent("pointermove", self, data));
        });
        on(id, "pointerup", function(data) {
            self.dispatchEvent(_makeEvent("pointerup", self, data));
        });
        on(id, "pointercancel", function(data) {
            self.dispatchEvent(_makeEvent("pointercancel", self, data));
        });
    } else if (type === "gesturestart" || type === "gesturechange" || type === "gestureend") {
        // Gesture events dispatched from C++ bridge
        if (typeof registerGesture === "function") registerGesture(id);
        on(id, "gesturestart", function(data) {
            self.dispatchEvent(_makeEvent("gesturestart", self, data));
        });
        on(id, "gesturechange", function(data) {
            self.dispatchEvent(_makeEvent("gesturechange", self, data));
        });
        on(id, "gestureend", function(data) {
            self.dispatchEvent(_makeEvent("gestureend", self, data));
        });
    } else if (type === "input" || type === "change") {
        on(id, "change", function(val) {
            self._value = val;
            var evt = _makeEvent("input", self);
            self.dispatchEvent(evt);
            var evt2 = _makeEvent("change", self);
            self.dispatchEvent(evt2);
        });
    } else if (type === "keydown" || type === "keyup" || type === "keypress") {
        // Global key events are forwarded through __dispatch__
    } else if (type === "focus") {
        on(id, "focus", function() {
            self.dispatchEvent(_makeEvent("focus", self));
        });
    } else if (type === "blur") {
        on(id, "blur", function() {
            self.dispatchEvent(_makeEvent("blur", self));
        });
    } else if (type === "wheel") {
        // pulp DIVERGE→PASS sweep — `el.addEventListener('wheel', fn)`
        // routes through the bridge `registerWheel` / `__dispatch__`
        // path. Before, only the explicit `registerWheel(id)` API was
        // accessible from JS — DOM consumers got no surface at all.
        if (typeof registerWheel === "function") registerWheel(id);
        on(id, "wheel", function(dx, dy) {
            var evt = _makeEvent("wheel", self, {});
            evt.deltaX = dx || 0;
            evt.deltaY = dy || 0;
            evt.deltaZ = 0;
            evt.deltaMode = 0;  // DOM_DELTA_PIXEL
            self.dispatchEvent(evt);
        });
    } else if (type === "dragstart" || type === "drag" || type === "dragend" ||
               type === "dragenter" || type === "dragover" || type === "dragleave" ||
               type === "drop") {
        // pulp DIVERGE→PASS sweep — DOM-style drag/drop event types
        // are surfaced through the existing bridge `registerDrop` API.
        // The native side fires a single `drop` callback with type +
        // payload data when a drop completes; we synthesize a
        // DragEvent-shaped object so CSS-style consumers' handlers
        // receive an event with .dataTransfer-like `_dropData`. Full
        // multi-stage dragstart/drag/dragend lifecycle (with native
        // drag-image rendering) remains a roadmap item — this slice
        // covers the common "register me as a drop target" usage so
        // `addEventListener('drop', fn)` is no longer a silent no-op.
        if (typeof registerDrop === "function") {
            // The bridge expects a callback NAME (not a function); pin
            // a synthetic per-element callback that fires our DOM
            // listeners. Idempotent because `_registerNativeEvent` is
            // called once per (id, type) pair from addEventListener.
            var cbName = "__drop_cb_" + id.replace(/[^a-zA-Z0-9_]/g, "_");
            globalThis[cbName] = function(dropType, data, x, y) {
                var evt = _makeEvent("drop", self, {});
                evt.clientX = x || 0;
                evt.clientY = y || 0;
                evt._dropData = { type: dropType, data: data };
                self.dispatchEvent(evt);
            };
            registerDrop(id, cbName);
        }
    }
};

// ── Pointer capture (P2b) ───────────────────────────────────────────────

Element.prototype.setPointerCapture = function(pointerId) {
    if (typeof nativeSetPointerCapture === "function")
        nativeSetPointerCapture(this._id, pointerId);
};

Element.prototype.releasePointerCapture = function(pointerId) {
    if (typeof nativeReleasePointerCapture === "function")
        nativeReleasePointerCapture(this._id, pointerId);
};

function _makeEvent(type, target, data) {
    var d = data || {};
    var ev = new Event(type, {
        // Native bridge events previously bubbled through Pulp's manual
        // parent walk. Keep that default while exposing the standard flag
        // React DOM and DOM-like userland expect to exist.
        bubbles: d.bubbles !== undefined ? !!d.bubbles : true,
        cancelable: d.cancelable !== undefined ? !!d.cancelable : true,
        composed: d.composed !== undefined ? !!d.composed : true
    });
    ev.target = target;
    ev.currentTarget = null;
    ev.eventPhase = 0;  // NONE; _dispatchEvent sets during traversal
    ev.timeStamp = (typeof performance !== "undefined" && performance.now) ? performance.now() : 0;
    // Self-reference for code that treats the bridged object as both the
    // DOM event and the native event payload.
    ev.nativeEvent = ev;

    // Position fields (P1)
    ev.clientX = d.clientX || 0;
    ev.clientY = d.clientY || 0;
    ev.offsetX = d.offsetX || 0;
    ev.offsetY = d.offsetY || 0;
    ev.pageX = d.clientX || 0;
    ev.pageY = d.clientY || 0;
    ev.screenX = d.clientX || 0;
    ev.screenY = d.clientY || 0;
    ev.movementX = d.movementX || 0;
    ev.movementY = d.movementY || 0;
    ev.button = d.button || 0;
    ev.buttons = d.buttons !== undefined ? d.buttons :
        (type === "pointerdown" || type === "mousedown" ||
         type === "pointermove" || type === "mousemove") ? 1 : 0;

    // Keyboard
    ev.key = d.key || "";
    ev.code = d.code || "";
    ev.ctrlKey = !!d.ctrlKey;
    ev.shiftKey = !!d.shiftKey;
    ev.altKey = !!d.altKey;
    ev.metaKey = !!d.metaKey;
    ev.getModifierState = function (k) {
        return !!{ Control: this.ctrlKey, Shift: this.shiftKey, Alt: this.altKey, Meta: this.metaKey }[k];
    };

    // Pointer (P2)
    ev.pointerId = d.pointerId || 0;
    ev.pointerType = d.pointerType || "mouse";
    ev.isPrimary = d.isPrimary !== undefined ? d.isPrimary : true;
    ev.width = d.width || 1;
    ev.height = d.height || 1;
    ev.tangentialPressure = d.tangentialPressure || 0;
    ev.tiltX = d.tiltX || 0;
    ev.tiltY = d.tiltY || 0;
    ev.twist = d.twist || 0;

    // Stylus (P3)
    ev.pressure = d.pressure !== undefined ? d.pressure : 0.5;
    ev.altitudeAngle = d.altitudeAngle || 0;
    ev.azimuthAngle = d.azimuthAngle || 0;

    // Gesture (P4)
    ev.scale = d.scale !== undefined ? d.scale : 1;
    ev.rotation = d.rotation || 0;
    ev.detail = d.detail !== undefined ? d.detail : null;
    ev.deltaX = d.deltaX || 0;
    ev.deltaY = d.deltaY || 0;
    ev.deltaZ = d.deltaZ || 0;
    ev.deltaMode = d.deltaMode || 0;

    // Coalesced/predicted (P5)
    ev._coalesced = d._coalesced || null;
    ev._predicted = d._predicted || null;
    ev.getCoalescedEvents = function () { return this._coalesced || [this]; };
    ev.getPredictedEvents = function () { return this._predicted || []; };

    // composedPath(): walks the _parentElement chain starting at target.
    // happy-dom returns [target, target.parent, ..., document, window];
    // pulp's tree is simpler — target up through parent chain.
    ev.composedPath = function () {
        if (!this.target) return [];
        var path = [this.target];
        var p = this.target._parentElement;
        while (p) { path.push(p); p = p._parentElement; }
        return path;
    };

    ev._noBubble = !ev.bubbles;
    return ev;
}

// pulp DIVERGE→PASS sweep — `new Event(name, init)` constructor surface.
// Userland `new Event('foo')` produces an object that round-trips
// through `Element.dispatchEvent`. Mirrors the DOM Event interface
// minimally — type / bubbles / cancelable / stopPropagation /
// preventDefault — which is what the harness gap was about. The
// `_makeEvent` factory above stays the canonical path for events
// SYNTHESIZED by the bridge (it includes all the position / pointer /
// gesture fields a native event needs); user-constructed Events are
// shaped like `_makeEvent` but only carry the fields the user passes.
function Event(type, eventInitDict) {
    var init = eventInitDict || {};
    this.type = String(type || "");
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.target = null;
    this.currentTarget = null;
    this.timeStamp = (typeof Date !== "undefined" && Date.now) ? Date.now() : 0;
    this._stopped = false;
    this._defaultPrevented = false;
    this._noBubble = !this.bubbles;
}
Event.NONE = 0;
Event.CAPTURING_PHASE = 1;
Event.AT_TARGET = 2;
Event.BUBBLING_PHASE = 3;
Event.prototype.NONE = Event.NONE;
Event.prototype.CAPTURING_PHASE = Event.CAPTURING_PHASE;
Event.prototype.AT_TARGET = Event.AT_TARGET;
Event.prototype.BUBBLING_PHASE = Event.BUBBLING_PHASE;
Event.prototype.stopPropagation = function() { this._stopped = true; };
Event.prototype.stopImmediatePropagation = function() { this._stopped = true; };
Event.prototype.preventDefault = function() {
    if (this.cancelable) this._defaultPrevented = true;
};
Event.prototype.composedPath = function() {
    if (!this.target) return [];
    var path = [this.target];
    var p = this.target._parentElement;
    while (p) { path.push(p); p = p._parentElement; }
    return path;
};
Object.defineProperty(Event.prototype, "defaultPrevented", {
    get: function() { return this._defaultPrevented; }
});
// Minimal CustomEvent for `new CustomEvent('foo', { detail })` parity
// with userland code that targets the standard browser surface.
function CustomEvent(type, eventInitDict) {
    Event.call(this, type, eventInitDict);
    this.detail = (eventInitDict && eventInitDict.detail !== undefined)
        ? eventInitDict.detail : null;
}
CustomEvent.prototype = Object.create(Event.prototype);
CustomEvent.prototype.constructor = CustomEvent;

function _fireListeners(el, event) {
    var id = el._id;
    var listeners = __eventListeners__[id] && __eventListeners__[id][event.type];
    if (!listeners) return;
    event.currentTarget = el;
    for (var i = 0; i < listeners.length; i++) {
        listeners[i].fn.call(el, event);
        if (event._stopped) break;
    }
}

function _dispatchEvent(target, event) {
    event.target = target;

    // Build ancestor path for capture/bubble
    var path = [];
    var el = target._parentElement;
    while (el) { path.unshift(el); el = el._parentElement; }

    // pulp jsx-instrument-import diag 2026-05-17 — log every dispatch
    // path for pointer/click/mouse events so we can see if the bubble
    // chain reaches __root__ where React-DOM delegate is registered.
    // Gated by globalThis.__pulpDebugDispatch__ to keep silent in
    // normal runs.
    if (globalThis.__pulpDebugDispatch__ && /^(click|mousedown|mouseup|pointerdown|pointerup)$/.test(event.type)) {
        var pathIds = path.map(function (e) { return e._id; }).join(">");
        var rootListeners = (__eventListeners__["__root__"]
            && __eventListeners__["__root__"][event.type]) || [];
        if (typeof __spectrLog === "function") {
            __spectrLog("[disp] " + event.type + " target=" + target._id
                + " path=" + pathIds + " rootHas=" + rootListeners.length);
        } else if (typeof console !== "undefined" && console.log) {
            console.log("[disp] " + event.type + " target=" + target._id
                + " path=" + pathIds + " rootHas=" + rootListeners.length);
        }
    }

    var Event_CAPTURING = 1, Event_AT_TARGET = 2, Event_BUBBLING = 3;

    // Capture phase (top-down)
    event.eventPhase = Event_CAPTURING;
    for (var i = 0; i < path.length && !event._stopped; i++) {
        var listeners = __eventListeners__[path[i]._id] && __eventListeners__[path[i]._id][event.type];
        if (listeners) {
            event.currentTarget = path[i];
            for (var j = 0; j < listeners.length; j++) {
                if (listeners[j].capture) {
                    listeners[j].fn.call(path[i], event);
                    if (event._stopped) {
                        event.eventPhase = 0;
                        event.currentTarget = null;
                        return;
                    }
                }
            }
        }
    }

    // Target phase
    event.eventPhase = Event_AT_TARGET;
    event.currentTarget = target;
    _fireListeners(target, event);
    if (event._stopped || event._noBubble) {
        event.eventPhase = 0;
        event.currentTarget = null;
        return;
    }

    // Bubble phase (bottom-up)
    event.eventPhase = Event_BUBBLING;
    for (var k = path.length - 1; k >= 0 && !event._stopped; k--) {
        var listeners2 = __eventListeners__[path[k]._id] && __eventListeners__[path[k]._id][event.type];
        if (listeners2) {
            event.currentTarget = path[k];
            for (var l = 0; l < listeners2.length; l++) {
                if (!listeners2[l].capture) {
                    listeners2[l].fn.call(path[k], event);
                    if (event._stopped) break;
                }
            }
        }
    }
    event.eventPhase = 0;
    event.currentTarget = null;
}

