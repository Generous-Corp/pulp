// ═══════════════════════════════════════════════════════════════════════════════
// Events + pointer-capture support
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
//   2. Pointer capture. Element.prototype setPointerCapture /
//      releasePointerCapture against the document-scoped pointer-capture
//      registry, plus the pointercancel synthetic event the bridge dispatches
//      when capture is released.
//
// Embed order: loaded AFTER web-compat-element.js so the Element constructor
// + prototype are already defined when these prototype overrides install.
// The functions on Element.prototype here can reference each other freely;
// cross-prelude resolution happens at call time, not parse time.

// ── Events ───────────────────────────────────────────────────────────────────

function __forgetWidgetCallbacks__(ids, options) {
    if (!ids || typeof ids.length !== "number") return;
    var preserveDomElementState = !!(options && options.preserveDomElementState);
    for (var i = 0; i < ids.length; i++) {
        var id = String(ids[i]);
        var prefix = id + ":";
        var element = (typeof __nativeElements__ !== "undefined") ? __nativeElements__[id] : null;
        for (var key in __callbacks__) {
            if (key.indexOf(prefix) === 0) delete __callbacks__[key];
        }
        for (var nativeKey in __nativeRegistered__) {
            if (nativeKey.indexOf(prefix) === 0) delete __nativeRegistered__[nativeKey];
        }
        if (preserveDomElementState && element) {
            element._autoEventsRegistered = false;
            element._nativeCreated = false;
            __invalidateStyleCache__(element);
            __nativeElements__[id] = element;
        } else {
            delete __eventListeners__[id];
            delete __nativeElements__[id];
        }
    }
}

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
    return !event.defaultPrevented;
};

Element.prototype.click = function() {
    if (!this.__pulpProgrammaticActivation
        && typeof globalThis.__pulpActivateMaterializedElement__ === "function") {
        var sequence = (globalThis.__pulpProgrammaticClickSequence__ || 0) + 1;
        globalThis.__pulpProgrammaticClickSequence__ = sequence;
        var token = "pulp-click-" + sequence;
        this.setAttribute("data-pulp-programmatic-click", token);
        this.__pulpProgrammaticActivation = true;
        try {
            if (globalThis.__pulpActivateMaterializedElement__(
                    '[data-pulp-programmatic-click="' + token + '"]',
                    "click", null)) return;
        } finally {
            this.__pulpProgrammaticActivation = false;
            this.removeAttribute("data-pulp-programmatic-click");
        }
    }
    if (typeof __dispatch__ === "function") {
        __dispatch__(this._id, "click",
            { bubbles: true, cancelable: true, button: 0, buttons: 0 });
        return;
    }
    this.dispatchEvent(_makeEvent("click", this,
        { bubbles: true, cancelable: true, button: 0, buttons: 0 }));
};

// focus() / blur() move NATIVE keyboard focus as well as the DOM bookkeeping.
// Without the native half, focusing an <input> set activeElement and fired a
// focus event while the TextEditor behind it never received a caret or a key.
// setFocus()/clearFocus() run the same transfer a pointer press does, and
// refuse views that cannot take focus (not focusable, disabled, hidden), so a
// focus() on a plain container leaves the focused field alone.
Element.prototype.focus = function() {
    var navigationClaim = this.getAttribute && this.getAttribute("aria-haspopup")
        && this.getAttribute("data-pulp-popup-default") !== "off"
        && typeof claimDocumentNavigationFocus === "function";
    // A popup trigger hands keyboard focus to the document navigation owner
    // instead, so giving the trigger native focus first would only be undone.
    if (!navigationClaim && this._nativeCreated && typeof setFocus === "function")
        setFocus(this._id);
    if (typeof document !== "undefined") document.activeElement = this;
    if (navigationClaim) claimDocumentNavigationFocus();
    this.dispatchEvent(_makeEvent("focus", this, { bubbles: false }));
};

Element.prototype.blur = function() {
    if (this._nativeCreated && typeof clearFocus === "function")
        clearFocus(this._id);
    if (typeof document !== "undefined" && document.activeElement === this)
        document.activeElement = null;
    this.dispatchEvent(_makeEvent("blur", this, { bubbles: false }));
};

// ── Mount-time focus: `autofocus` and the dialog default ────────────────────
//
// Runs when a subtree becomes connected to the document (appendChild /
// insertBefore under document.body). Mirrors the browser rule for the
// `autofocus` attribute: the FIRST not-yet-processed autofocus element in the
// newly connected subtree takes focus, once. Every autofocus element seen is
// marked processed, so re-inserting or re-rendering never steals focus again.
//
// When the subtree has no autofocus element, a mounted dialog (role="dialog",
// role="alertdialog", or aria-modal="true") focuses its first enabled text
// field, so a dialog that opens with a text field gets a live caret without
// app code. Opt out per dialog, or app-wide on any ancestor such as
// document.body, with data-pulp-autofocus="off". A dialog mounted hidden is
// left unprocessed: the default applies at mount, not when CSS later shows
// it. An open <dialog> counts as a dialog, and <dialog>.show() / showModal()
// run the same focusing step each time they open one.
var __pulpTextFieldTypes__ = {
    "": true, text: true, search: true, email: true, url: true, tel: true,
    password: true
};

function __pulpIsTextField__(el) {
    if (!el || !el.tagName) return false;
    if (el.tagName === "TEXTAREA") return true;
    if (el.tagName !== "INPUT") return false;
    var type = String(el._type || (el.getAttribute && el.getAttribute("type")) || "")
        .toLowerCase();
    return __pulpTextFieldTypes__[type] === true;
}

function __pulpIsDialog__(el) {
    if (!el || !el.getAttribute) return false;
    if (el.tagName === "DIALOG") return !!el._dialogOpen;
    var role = el.getAttribute("role");
    return role === "dialog" || role === "alertdialog"
        || el.getAttribute("aria-modal") === "true";
}

function __pulpHasAutofocus__(el) {
    return !!(el && el.hasAttribute && el.hasAttribute("autofocus"));
}

function __pulpConnectedToDocument__(el) {
    var body = typeof __bodyElement__ !== "undefined" ? __bodyElement__ : null;
    var html = typeof __documentElement__ !== "undefined" ? __documentElement__ : null;
    for (var cur = el; cur; cur = cur._parentElement) {
        if (cur === body || cur === html) return true;
    }
    return false;
}

function __pulpRenderedFrom__(el, stop) {
    for (var cur = el; cur && cur !== stop; cur = cur._parentElement) {
        if (cur._hidden || cur._disabled) return false;
        if (cur.style && cur.style.display === "none") return false;
    }
    return true;
}

function __pulpAutofocusOptedOut__(el) {
    for (var cur = el; cur; cur = cur._parentElement) {
        if (cur.getAttribute && cur.getAttribute("data-pulp-autofocus") === "off")
            return true;
    }
    return false;
}

function __pulpWalkElements__(root, visit) {
    var stack = [root];
    while (stack.length) {
        var el = stack.pop();
        if (visit(el) === false) return;
        var kids = el._children;
        if (!kids) continue;
        for (var i = kids.length - 1; i >= 0; --i) stack.push(kids[i]);
    }
}

function __pulpFirstTextField__(root) {
    var found = null;
    __pulpWalkElements__(root, function(el) {
        if (el !== root && __pulpIsTextField__(el) && !el._disabled
            && __pulpRenderedFrom__(el, root)) {
            found = el;
            return false;
        }
    });
    return found;
}

// The element a dialog should focus when it opens: its first autofocus
// descendant, else (unless opted out) its first text field.
function __pulpDialogFocusTarget__(dialog) {
    var explicit = null;
    __pulpWalkElements__(dialog, function(el) {
        if (el !== dialog && __pulpHasAutofocus__(el)) { explicit = el; return false; }
    });
    if (explicit) return explicit;
    if (__pulpAutofocusOptedOut__(dialog)) return null;
    return __pulpFirstTextField__(dialog);
}

// <dialog>.show() / showModal() on a connected dialog run the dialog focusing
// step every time it opens, as in a browser.
function __pulpFocusOpenedDialog__(dialog) {
    if (!dialog || !__pulpConnectedToDocument__(dialog)) return null;
    dialog.__pulpDialogFocusProcessed = true;
    var target = __pulpDialogFocusTarget__(dialog);
    if (target && typeof target.focus === "function") target.focus();
    return target;
}

function __pulpApplyMountFocus__(root) {
    if (!root || !__pulpConnectedToDocument__(root)) return null;
    var target = null;
    var dialogs = [];
    __pulpWalkElements__(root, function(el) {
        if (__pulpHasAutofocus__(el) && !el.__pulpAutofocusProcessed) {
            el.__pulpAutofocusProcessed = true;
            if (!target) target = el;
        }
        if (__pulpIsDialog__(el) && !el.__pulpDialogFocusProcessed)
            dialogs.push(el);
    });
    if (!target) {
        for (var i = 0; i < dialogs.length && !target; ++i) {
            var dialog = dialogs[i];
            if (!__pulpRenderedFrom__(dialog, null)) continue;
            dialog.__pulpDialogFocusProcessed = true;
            // An autofocus element inside this dialog was already processed
            // by an earlier mount; the dialog default must not override it.
            var hasOwnAutofocus = false;
            __pulpWalkElements__(dialog, function(el) {
                if (__pulpHasAutofocus__(el)) { hasOwnAutofocus = true; return false; }
            });
            if (hasOwnAutofocus || __pulpAutofocusOptedOut__(dialog)) continue;
            target = __pulpFirstTextField__(dialog);
        }
    } else {
        for (var d = 0; d < dialogs.length; ++d)
            dialogs[d].__pulpDialogFocusProcessed = true;
    }
    if (target && typeof target.focus === "function") target.focus();
    return target;
}

Element.prototype._registerNativeEvent = function(type) {
    var id = this._id;
    var self = this;
    if (type === "click" || type === "mousedown" || type === "mouseup") {
        registerClick(id);
        // __dispatch__ owns the one Element::dispatchEvent entry. This
        // callback preserves the low-level registration slot without entering
        // the DOM a second time.
        on(id, "click", function() {});
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
        // Native pointer delivery has exactly one DOM origin. Keep no-op
        // low-level callbacks for native ancestors and let __dispatch__ perform
        // the sole Element dispatch + JS capture/bubble walk.
        on(id, "pointerdown", function() {});
        on(id, "pointermove", function() {});
        on(id, "pointerup", function() {});
        on(id, "pointercancel", function() {});
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
        // `el.addEventListener('wheel', fn)` routes through the bridge
        // `registerWheel` / `__dispatch__` path so DOM consumers can use
        // the standard listener surface instead of the explicit
        // `registerWheel(id)` API.
        if (typeof registerWheel === "function") registerWheel(id);
        // __dispatch__ owns the single DOM element fan-out. Keep this callback
        // registered so the native channel exists, but do not dispatch here as
        // well or one native wheel tick reaches listeners twice.
        on(id, "wheel", function() {});
    } else if (type === "dragstart" || type === "drag" || type === "dragend" ||
               type === "dragenter" || type === "dragover" || type === "dragleave" ||
               type === "drop") {
        // DOM-style drag/drop event types are surfaced through the existing
        // bridge `registerDrop` API.
        // The native side fires a single `drop` callback with type +
        // payload data when a drop completes; we synthesize a
        // DragEvent-shaped object so CSS-style consumers' handlers
        // receive an event with .dataTransfer-like `_dropData`. This covers
        // the common "register me as a drop target" usage so
        // `addEventListener('drop', fn)` is no longer a silent no-op; it does
        // not implement a full multi-stage drag lifecycle or native drag-image
        // rendering.
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

// ── Pointer capture ─────────────────────────────────────────────────────

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

    // Position fields
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

    // Pointer
    ev.pointerId = d.pointerId || 0;
    ev.pointerType = d.pointerType || "mouse";
    ev.isPrimary = d.isPrimary !== undefined ? d.isPrimary : true;
    ev.width = d.width || 1;
    ev.height = d.height || 1;
    ev.tangentialPressure = d.tangentialPressure || 0;
    ev.tiltX = d.tiltX || 0;
    ev.tiltY = d.tiltY || 0;
    ev.twist = d.twist || 0;

    // Stylus
    ev.pressure = d.pressure !== undefined ? d.pressure : 0.5;
    ev.altitudeAngle = d.altitudeAngle || 0;
    ev.azimuthAngle = d.azimuthAngle || 0;

    // Gesture
    ev.scale = d.scale !== undefined ? d.scale : 1;
    ev.rotation = d.rotation || 0;
    ev.detail = d.detail !== undefined ? d.detail : null;
    ev.deltaX = d.deltaX || 0;
    ev.deltaY = d.deltaY || 0;
    ev.deltaZ = d.deltaZ || 0;
    ev.deltaMode = d.deltaMode || 0;

    // Coalesced/predicted
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

// `new Event(name, init)` produces an object that round-trips through
// `Element.dispatchEvent`. Mirrors the DOM Event interface
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
    this._stoppedImmediate = false;
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
Event.prototype.stopImmediatePropagation = function() {
    this._stopped = true;
    this._stoppedImmediate = true;
};
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
        if (event._stoppedImmediate) break;
    }
}

function _dispatchEvent(target, event) {
    event.target = target;

    // Build ancestor path for capture/bubble
    var path = [];
    var el = target._parentElement;
    while (el) { path.unshift(el); el = el._parentElement; }

    // Debug-only dispatch logging for pointer/click/mouse events. Shows whether
    // the bubble chain reaches __root__ where the React-DOM delegate is
    // registered. Gated by globalThis.__pulpDebugDispatch__ to keep normal runs
    // silent.
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
                    if (event._stoppedImmediate) break;
                }
            }
            if (event._stopped) {
                event.eventPhase = 0;
                event.currentTarget = null;
                return;
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
                    if (event._stoppedImmediate) break;
                }
            }
        }
    }
    event.eventPhase = 0;
    event.currentTarget = null;

    // Pulp's popup default-behavior owner watches for the gesture that opens an
    // `aria-haspopup` menu. `__dispatch__` fans only pointer events on to
    // `document`, deliberately, so a native click never reaches that owner
    // through the document path — and an app that opens its menu in a `click`
    // handler would leave the open menu unowned. Offer the element-path event
    // here instead, where the target is the real one, and mark it so the
    // document tail cannot handle the same event a second time.
    if (!event.defaultPrevented && !event.__pulpPopupOffered
        && typeof globalThis.__pulpPopupDefaultHandle__ === "function") {
        event.__pulpPopupOffered = true;
        globalThis.__pulpPopupDefaultHandle__(event);
    }
}
