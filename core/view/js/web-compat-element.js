// web-compat.js — Web-native authoring layer for Pulp
// Loaded as a prelude after css-colors.js and css-parser.js
// Depends on: bridge functions (createRow, createCol, createLabel, setFlex, etc.)
//             css-parser.js (parseCSSLength, parseCSSColor, expandShorthand, parseTransform, parseTransition)
//             css-colors.js (__cssColors__)

// ═══════════════════════════════════════════════════════════════════════════════
// Internal state
// ═══════════════════════════════════════════════════════════════════════════════

var __nextId__ = 1;
var __elements__ = {};          // id -> Element
var __classIndex__ = {};        // className -> Set of element ids
var __stylesheets__ = [];       // attached StyleSheet instances
var __eventListeners__ = {};    // id -> { eventType -> [{fn, capture}] }

function __genId__() { return "__el_" + (__nextId__++) + "__"; }

// ═══════════════════════════════════════════════════════════════════════════════
// Element class
// ═══════════════════════════════════════════════════════════════════════════════

function Element(tagName, nativeId) {
    this.tagName = (tagName || "div").toUpperCase();
    this._id = nativeId || __genId__();
    this._userIdSet = false;
    this._className = "";
    this._classList = new ClassList(this);
    this._children = [];
    this._parentElement = null;
    this._textContent = "";
    this._value = "";
    this._hidden = false;
    this._disabled = false;
    this._type = "";           // for input elements
    this._min = 0;
    this._max = 100;
    this._checked = false;
    this._placeholder = "";
    this._nativeCreated = false;
    this._attributes = {};
    this._dataset = {};
    this.style = new CSSStyleDeclaration(this);
}

// Create the native widget based on tag + type
Element.prototype._ensureNative = function() {
    if (this._nativeCreated) return;
    this._nativeCreated = true;

    var tag = this.tagName.toLowerCase();
    var id = this._id;

    if (tag === "div" || tag === "section" || tag === "article" || tag === "aside" ||
        tag === "header" || tag === "footer" || tag === "nav" || tag === "main") {
        createCol(id, "");
    } else if (tag === "span" || tag === "p" || tag === "label") {
        createLabel(id, "", "");
    } else if (tag === "svg") {
        // pulp #1147 — inline SVGs in web-compat code (Spectr's mode-icon
        // popover rows, React-rendered icons) are leaf containers. We
        // don't ship an SVG renderer, but we MUST honor the HTML
        // `width`/`height` attributes so the flex parent reserves layout
        // space. Without this the row collapses to height:0 and the
        // sibling text paints over a blank gutter. Width/height attribute
        // replay happens in the shared block below.
        createCol(id, "");
    } else if (tag === "h1") {
        createLabel(id, "", "");
        setFontSize(id, 32); setFontWeight(id, 700);
    } else if (tag === "h2") {
        createLabel(id, "", "");
        setFontSize(id, 24); setFontWeight(id, 700);
    } else if (tag === "h3") {
        createLabel(id, "", "");
        setFontSize(id, 20); setFontWeight(id, 600);
    } else if (tag === "h4") {
        createLabel(id, "", "");
        setFontSize(id, 16); setFontWeight(id, 600);
    } else if (tag === "h5") {
        createLabel(id, "", "");
        setFontSize(id, 14); setFontWeight(id, 600);
    } else if (tag === "h6") {
        createLabel(id, "", "");
        setFontSize(id, 12); setFontWeight(id, 600);
    } else if (tag === "button") {
        createToggleButton(id, "");
    } else if (tag === "input") {
        var t = this._type || "text";
        if (t === "range") {
            createFader(id, "vertical", "");
        } else if (t === "checkbox") {
            createCheckbox(id, "");
        } else {
            createTextEditor(id, "");
            if (this._placeholder) setPlaceholder(id, this._placeholder);
        }
    } else if (tag === "textarea") {
        createTextEditor(id, "");
        setMultiLine(id, 1);
    } else if (tag === "select") {
        createCombo(id, "");
    } else if (tag === "canvas") {
        createCanvas(id, "");
    } else if (tag === "progress") {
        createProgress(id, "");
    } else if (tag === "hr") {
        createCol(id, "");
        setFlex(id, "height", 1);
        setBackground(id, "#666666");
    } else if (tag === "img") {
        createLabel(id, "", ""); // placeholder until image loading
    } else if (tag === "details") {
        createCol(id, "");
    } else if (tag === "dialog") {
        createPanel(id, "");
        setVisible(id, false);
    } else if (tag === "style") {
        // pulp #1323 — `<style>` is a non-rendered CSS source. We still
        // create a hidden native shell so DOM ops (appendChild,
        // textContent flush, removeChild) keep working uniformly, but
        // mark the element so its textContent / appended Text-node
        // children are routed through the CSS-rule translator instead
        // of `setText()`. The element itself never paints.
        this._isStyleElement = true;
        this._appliedSheet = null;
        createCol(id, "");
        setVisible(id, false);
    } else {
        // Unknown tag — create as container
        createCol(id, "");
    }

    // pulp #1147 — presentational `width`/`height` HTML attributes are
    // replayed via __replayMediaAttributes__ once the native widget is
    // mounted (called from appendChild / insertBefore / _ensureNative).
    if (typeof __replayMediaAttributes__ === "function") {
        __replayMediaAttributes__(this);
    }
};

// pulp #1147 — shared helper that maps presentational HTML attributes
// (width, height) on layout-leaf media tags (<svg>, <img>, <canvas>,
// <video>) to flex preferred sizing. Idempotent — safe to call from
// _ensureNative (createElement-then-flushAll path) AND from appendChild
// (React/JSX setAttribute-before-mount path). Inline `style.width` still
// wins because `_flushAll()` runs AFTER this replay in dom-ops.
function __replayMediaAttributes__(el) {
    if (!el || !el._nativeCreated || !el._attributes) return;
    var tag = el.tagName.toLowerCase();
    if (tag !== "svg" && tag !== "img" && tag !== "canvas" && tag !== "video") return;
    if (typeof setFlex !== "function") return;
    var w = el._attributes.width;
    var h = el._attributes.height;
    if (w !== undefined) {
        var pw = parseFloat(w); if (pw === pw) setFlex(el._id, "width", pw);
    }
    if (h !== undefined) {
        var ph = parseFloat(h); if (ph === ph) setFlex(el._id, "height", ph);
    }
}

// ── nodeType / nodeName (DOM Level 1 reconciler hooks) ──────────────────────
//
// React 18's reconciler reads `node.nodeType` (~55 call sites in
// react-dom.development.js) and `node.nodeName` (~15 sites) on every DOM
// mutation. Without these, the reconciler bails out before its first
// commit. See pulp #468 (gap matrix).
//
// Constants per the DOM Level 1 spec:
//   ELEMENT_NODE = 1, TEXT_NODE = 3, COMMENT_NODE = 8.
// We omit the rarely-used node types (DOCUMENT_NODE = 9 etc.) — react-dom
// only checks 1/3/8 in its hot paths.

Object.defineProperty(Element.prototype, "nodeType", {
    get: function() { return 1; }, // ELEMENT_NODE
    configurable: true
});

Object.defineProperty(Element.prototype, "nodeName", {
    // DOM spec: nodeName for Element is the upper-case tag name.
    // tagName is already upper-cased in the Element constructor.
    get: function() { return this.tagName; },
    configurable: true
});

// Attach the same numeric constants to the Element constructor so
// `node.ELEMENT_NODE === 1` style checks (also used by React) succeed.
Element.ELEMENT_NODE = 1;
Element.TEXT_NODE = 3;
Element.COMMENT_NODE = 8;
Element.prototype.ELEMENT_NODE = 1;
Element.prototype.TEXT_NODE = 3;
Element.prototype.COMMENT_NODE = 8;

// ── ID property ──────────────────────────────────────────────────────────────

Object.defineProperty(Element.prototype, "id", {
    get: function() { return this._userIdSet ? this._attributes["id"] || "" : ""; },
    set: function(v) {
        this._userIdSet = true;
        this._attributes["id"] = v;
        // Register for getElementById lookup
        __elements__["#" + v] = this;
    }
});

// ── className / classList ────────────────────────────────────────────────────

Object.defineProperty(Element.prototype, "className", {
    get: function() { return this._className; },
    set: function(v) {
        var old = this._className;
        this._className = v || "";
        this._updateClassIndex(old, this._className);
        this._reapplyStylesheets();
    }
});

Object.defineProperty(Element.prototype, "classList", {
    get: function() { return this._classList; }
});

Element.prototype._updateClassIndex = function(oldStr, newStr) {
    var oldClasses = oldStr ? oldStr.split(/\s+/).filter(Boolean) : [];
    var newClasses = newStr ? newStr.split(/\s+/).filter(Boolean) : [];
    var id = this._id;
    for (var i = 0; i < oldClasses.length; i++) {
        var c = oldClasses[i];
        if (__classIndex__[c]) __classIndex__[c].delete(id);
    }
    for (var j = 0; j < newClasses.length; j++) {
        var c2 = newClasses[j];
        if (!__classIndex__[c2]) __classIndex__[c2] = new Set();
        __classIndex__[c2].add(id);
    }
};

// ── ClassList ────────────────────────────────────────────────────────────────

function ClassList(el) { this._el = el; }

ClassList.prototype.add = function() {
    var classes = this._el._className ? this._el._className.split(/\s+/) : [];
    for (var i = 0; i < arguments.length; i++) {
        if (classes.indexOf(arguments[i]) < 0) classes.push(arguments[i]);
    }
    this._el.className = classes.join(" ");
};

ClassList.prototype.remove = function() {
    var classes = this._el._className ? this._el._className.split(/\s+/) : [];
    for (var i = 0; i < arguments.length; i++) {
        var idx = classes.indexOf(arguments[i]);
        if (idx >= 0) classes.splice(idx, 1);
    }
    this._el.className = classes.join(" ");
};

ClassList.prototype.toggle = function(c, force) {
    if (force !== undefined) {
        if (force) this.add(c); else this.remove(c);
        return force;
    }
    if (this.contains(c)) { this.remove(c); return false; }
    this.add(c); return true;
};

ClassList.prototype.contains = function(c) {
    return (" " + this._el._className + " ").indexOf(" " + c + " ") >= 0;
};

ClassList.prototype.toString = function() { return this._el._className; };

Object.defineProperty(ClassList.prototype, "length", {
    get: function() {
        return this._el._className ? this._el._className.split(/\s+/).filter(Boolean).length : 0;
    }
});

ClassList.prototype.item = function(i) {
    var classes = this._el._className ? this._el._className.split(/\s+/).filter(Boolean) : [];
    return classes[i] || null;
};

// ── textContent / value / hidden / disabled ──────────────────────────────────

Object.defineProperty(Element.prototype, "textContent", {
    get: function() { return this._textContent; },
    set: function(v) {
        this._textContent = v || "";
        // pulp #1323 — `<style>` element textContent is CSS source, not
        // a label. Route it through the rule translator. We deliberately
        // skip `setText()` so the element stays invisible in the layout.
        if (this.tagName === "STYLE" || this._isStyleElement) {
            if (typeof _processStyleElement === "function") {
                _processStyleElement(this);
            }
            return;
        }
        if (this._nativeCreated) {
            setText(this._id, this._textContent);
        }
    }
});

Object.defineProperty(Element.prototype, "value", {
    get: function() { return this._value; },
    set: function(v) {
        this._value = v;
        if (!this._nativeCreated) return;
        var tag = this.tagName.toLowerCase();
        if (tag === "input" && this._type === "range") {
            var norm = (parseFloat(v) - this._min) / (this._max - this._min);
            setValue(this._id, Math.max(0, Math.min(1, norm)));
        } else if (tag === "input" && this._type === "checkbox") {
            setValue(this._id, v ? 1 : 0);
        } else if (tag === "progress") {
            setProgress(this._id, parseFloat(v) || 0);
        } else {
            setText(this._id, String(v));
        }
    }
});

Object.defineProperty(Element.prototype, "hidden", {
    get: function() { return this._hidden; },
    set: function(v) {
        this._hidden = !!v;
        if (this._nativeCreated) setVisible(this._id, !this._hidden);
    }
});

Object.defineProperty(Element.prototype, "disabled", {
    get: function() { return this._disabled; },
    set: function(v) {
        this._disabled = !!v;
        this._reapplyStylesheets();
    }
});

// ── Input-specific properties ────────────────────────────────────────────────

Object.defineProperty(Element.prototype, "type", {
    get: function() { return this._type; },
    set: function(v) { this._type = v || "text"; }
});

Object.defineProperty(Element.prototype, "min", {
    get: function() { return this._min; },
    set: function(v) { this._min = parseFloat(v) || 0; }
});

Object.defineProperty(Element.prototype, "max", {
    get: function() { return this._max; },
    set: function(v) { this._max = parseFloat(v) || 100; }
});

Object.defineProperty(Element.prototype, "checked", {
    get: function() { return this._checked; },
    set: function(v) {
        this._checked = !!v;
        if (this._nativeCreated) setValue(this._id, v ? 1 : 0);
    }
});

Object.defineProperty(Element.prototype, "placeholder", {
    get: function() { return this._placeholder; },
    set: function(v) {
        this._placeholder = v || "";
        if (this._nativeCreated) setPlaceholder(this._id, this._placeholder);
    }
});

// ── DOM manipulation ─────────────────────────────────────────────────────────

Object.defineProperty(Element.prototype, "children", {
    get: function() { return this._children.slice(); }
});

Object.defineProperty(Element.prototype, "childNodes", {
    get: function() { return this._children.slice(); }
});

Object.defineProperty(Element.prototype, "firstChild", {
    get: function() { return this._children[0] || null; }
});

Object.defineProperty(Element.prototype, "lastChild", {
    get: function() { return this._children[this._children.length - 1] || null; }
});

Object.defineProperty(Element.prototype, "parentElement", {
    get: function() { return this._parentElement; }
});

Object.defineProperty(Element.prototype, "parentNode", {
    get: function() { return this._parentElement; }
});

Object.defineProperty(Element.prototype, "nextSibling", {
    get: function() {
        if (!this._parentElement) return null;
        var siblings = this._parentElement._children;
        var idx = siblings.indexOf(this);
        return idx >= 0 && idx < siblings.length - 1 ? siblings[idx + 1] : null;
    }
});

Object.defineProperty(Element.prototype, "previousSibling", {
    get: function() {
        if (!this._parentElement) return null;
        var siblings = this._parentElement._children;
        var idx = siblings.indexOf(this);
        return idx > 0 ? siblings[idx - 1] : null;
    }
});

// appendChild, removeChild, insertBefore, replaceChild, remove are bound
// in web-compat-dom-ops.js (a small file that stays under QuickJS's
// compilation stack limit)

Element.prototype.cloneNode = function(deep) {
    var clone = new Element(this.tagName.toLowerCase());
    clone._className = this._className;
    clone._textContent = this._textContent;
    clone._type = this._type;
    clone._value = this._value;
    // Copy style declarations
    for (var k in this.style._props) {
        clone.style._props[k] = this.style._props[k];
    }
    if (deep) {
        for (var i = 0; i < this._children.length; i++) {
            clone.appendChild(this._children[i].cloneNode(true));
        }
    }
    return clone;
};

// ── Attributes ───────────────────────────────────────────────────────────────

Element.prototype.setAttribute = function(name, value) {
    this._attributes[name] = String(value);
    if (name === "id") this.id = value;
    else if (name === "class") this.className = value;
    else if (name.indexOf("data-") === 0) {
        this._dataset[_camelCase(name.slice(5))] = value;
        // pulp #1148 (slice b) — `data-overlay="true"` is the explicit
        // author hint for the auto-overlay heuristic. Re-evaluate now
        // so the bridge sees the claim/release immediately rather than
        // waiting for an unrelated style mutation to drive it.
        if (name === "data-overlay" && this.style && this.style._reevaluateOverlay) {
            this.style._reevaluateOverlay();
        }
    }
    // pulp #1147 — HTML `width`/`height` attributes on layout-leaf
    // elements (<svg>, <img>, <canvas>, <video>) are presentational
    // dimensions per the HTML spec. JSX/React encodes inline SVG sizes
    // this way (`<svg width="28" height="20">`), so we MUST translate
    // these to flex preferred sizing or the element collapses to 0
    // and its row siblings have no anchor. The shared helper handles
    // both paths (createElement-then-mount and setAttribute-before-mount)
    // and is a no-op when the widget isn't created yet — appendChild
    // re-runs the replay once the native node exists.
    else if (name === "width" || name === "height") {
        if (typeof __replayMediaAttributes__ === "function") {
            __replayMediaAttributes__(this);
        }
    }
};

Element.prototype.getAttribute = function(name) {
    if (name === "id") return this.id;
    if (name === "class") return this.className;
    return this._attributes[name] !== undefined ? this._attributes[name] : null;
};

Element.prototype.removeAttribute = function(name) {
    var was = this._attributes[name];
    delete this._attributes[name];
    if (name.indexOf("data-") === 0) {
        delete this._dataset[_camelCase(name.slice(5))];
        // pulp #1148 (slice b) — clearing `data-overlay` may release
        // the auto-claim if no CSS shape still satisfies the heuristic.
        if (name === "data-overlay" && was !== undefined &&
            this.style && this.style._reevaluateOverlay) {
            this.style._reevaluateOverlay();
        }
    }
};

Element.prototype.hasAttribute = function(name) {
    return this._attributes[name] !== undefined;
};

Object.defineProperty(Element.prototype, "dataset", {
    get: function() { return this._dataset; }
});

function _camelCase(str) {
    return str.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
}

// ── getBoundingClientRect ────────────────────────────────────────────────────

Element.prototype.getBoundingClientRect = function() {
    if (!this._nativeCreated) return { x: 0, y: 0, width: 0, height: 0, top: 0, right: 0, bottom: 0, left: 0 };
    // Use native bridge if available, otherwise return zeros
    if (typeof getLayoutRect === "function") {
        var r = getLayoutRect(this._id);
        if (r) return r;
    }
    return { x: 0, y: 0, width: 0, height: 0, top: 0, right: 0, bottom: 0, left: 0 };
};

// ── offsetWidth / offsetHeight ───────────────────────────────────────────────

Object.defineProperty(Element.prototype, "offsetWidth", {
    get: function() { var r = this.getBoundingClientRect(); return r.width; }
});

Object.defineProperty(Element.prototype, "offsetHeight", {
    get: function() { var r = this.getBoundingClientRect(); return r.height; }
});

Object.defineProperty(Element.prototype, "clientWidth", {
    get: function() { return this.offsetWidth; }
});

Object.defineProperty(Element.prototype, "clientHeight", {
    get: function() { return this.offsetHeight; }
});

Object.defineProperty(Element.prototype, "ownerDocument", {
    get: function() {
        return typeof document !== "undefined" ? document : null;
    }
});

Element.prototype.getRootNode = function() {
    if (typeof document !== "undefined") return document;
    return this;
};

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
    return {
        type: type,
        target: target,
        currentTarget: null,
        // Position (P1)
        clientX: d.clientX || 0,
        clientY: d.clientY || 0,
        offsetX: d.offsetX || 0,
        offsetY: d.offsetY || 0,
        button: d.button || 0,
        // Keyboard
        key: d.key || "", code: d.code || "",
        ctrlKey: !!d.ctrlKey, shiftKey: !!d.shiftKey,
        altKey: !!d.altKey, metaKey: !!d.metaKey,
        // Pointer (P2)
        pointerId: d.pointerId || 0,
        pointerType: d.pointerType || "mouse",
        isPrimary: d.isPrimary !== undefined ? d.isPrimary : true,
        // Stylus (P3)
        pressure: d.pressure !== undefined ? d.pressure : 0.5,
        altitudeAngle: d.altitudeAngle || 0,
        azimuthAngle: d.azimuthAngle || 0,
        // Gesture (P4)
        scale: d.scale !== undefined ? d.scale : 1,
        rotation: d.rotation || 0,
        // Coalesced/predicted (P5)
        _coalesced: d._coalesced || null,
        _predicted: d._predicted || null,
        getCoalescedEvents: function() { return this._coalesced || [this]; },
        getPredictedEvents: function() { return this._predicted || []; },
        // Propagation control
        _stopped: false,
        _defaultPrevented: false,
        _noBubble: false,
        stopPropagation: function() { this._stopped = true; },
        preventDefault: function() { this._defaultPrevented = true; }
    };
}

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

    // Capture phase (top-down)
    for (var i = 0; i < path.length && !event._stopped; i++) {
        var listeners = __eventListeners__[path[i]._id] && __eventListeners__[path[i]._id][event.type];
        if (listeners) {
            event.currentTarget = path[i];
            for (var j = 0; j < listeners.length; j++) {
                if (listeners[j].capture) {
                    listeners[j].fn.call(path[i], event);
                    if (event._stopped) return;
                }
            }
        }
    }

    // Target phase
    _fireListeners(target, event);
    if (event._stopped || event._noBubble) return;

    // Bubble phase (bottom-up)
    for (var k = path.length - 1; k >= 0 && !event._stopped; k--) {
        var listeners2 = __eventListeners__[path[k]._id] && __eventListeners__[path[k]._id][event.type];
        if (listeners2) {
            event.currentTarget = path[k];
            for (var l = 0; l < listeners2.length; l++) {
                if (!listeners2[l].capture) {
                    listeners2[l].fn.call(path[k], event);
                    if (event._stopped) return;
                }
            }
        }
    }
}

// ── Stylesheet re-application ────────────────────────────────────────────────

Element.prototype._reapplyStylesheets = function() {
    for (var i = 0; i < __stylesheets__.length; i++) {
        __stylesheets__[i]._applyTo(this);
    }
};

// ── Native reparenting helper ────────────────────────────────────────────────

function _reparentNative(child, parentId) {
    var tag = child.tagName.toLowerCase();
    var id = child._id;

    // Re-create the widget under the new parent
    if (tag === "div" || tag === "section" || tag === "article" || tag === "aside" ||
        tag === "header" || tag === "footer" || tag === "nav" || tag === "main") {
        createCol(id, parentId);
    } else if (tag === "span" || tag === "p" || tag === "label" ||
               tag === "h1" || tag === "h2" || tag === "h3" ||
               tag === "h4" || tag === "h5" || tag === "h6") {
        createLabel(id, child._textContent || "", parentId);
    } else if (tag === "svg") {
        // pulp #1147 — same reasoning as _ensureNative: keep the
        // SVG node as a layout container so child elements still
        // attach. The width/height attributes are replayed by the
        // shared helper at the end of this function.
        createCol(id, parentId);
    } else if (tag === "button") {
        createToggleButton(id, parentId);
    } else if (tag === "input") {
        var t = child._type || "text";
        if (t === "range") createFader(id, "vertical", parentId);
        else if (t === "checkbox") createCheckbox(id, parentId);
        else createTextEditor(id, parentId);
    } else if (tag === "textarea") {
        createTextEditor(id, parentId);
        setMultiLine(id, 1);
    } else if (tag === "select") {
        createCombo(id, parentId);
    } else if (tag === "canvas") {
        createCanvas(id, parentId);
    } else if (tag === "progress") {
        createProgress(id, parentId);
    } else if (tag === "hr") {
        createCol(id, parentId);
        setFlex(id, "height", 1);
        setBackground(id, "#666666");
    } else if (tag === "img") {
        createLabel(id, "", parentId);
    } else if (tag === "dialog") {
        createPanel(id, parentId);
        setVisible(id, false);
    } else {
        createCol(id, parentId);
    }

    child._nativeCreated = true;

    // pulp #1147 — replay presentational attributes after the native
    // node is recreated so the new flex sizing matches the original.
    if (typeof __replayMediaAttributes__ === "function") {
        __replayMediaAttributes__(child);
    }

    // Recursively reparent children
    for (var i = 0; i < child._children.length; i++) {
        var c = child._children[i];
        if (c._nativeCreated) removeWidget(c._id);
        _reparentNative(c, id);
        if (c._textContent) setText(c._id, c._textContent);
        c.style._flushAll();
    }
}
