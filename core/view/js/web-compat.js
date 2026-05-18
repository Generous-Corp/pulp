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
    } else {
        // Unknown tag — create as container
        createCol(id, "");
    }
};

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

ClassList.prototype.replace = function(oldToken, newToken) {
    if (!this.contains(oldToken)) return false;
    this.remove(oldToken);
    this.add(newToken);
    return true;
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

Element.prototype.appendChild = function(child) {
    if (!(child instanceof Element)) return child;
    // Remove from old parent
    if (child._parentElement) child._parentElement.removeChild(child);
    child._parentElement = this;
    this._children.push(child);
    // Ensure parent native widget exists
    this._ensureNative();
    if (!child._nativeCreated) {
        // First time: create directly under this parent (no remove+re-create)
        _reparentNative(child, this._id);
        if (child._textContent) setText(child._id, child._textContent);
        // Defer style flush to reduce call stack depth
        // child.style._flushAll();
        // child._reapplyStylesheets();
    } else {
        // Already in tree: remove and re-parent
        removeWidget(child._id);
        _reparentNative(child, this._id);
        if (child._textContent) setText(child._id, child._textContent);
        child.style._flushAll();
        child._reapplyStylesheets();
    }
    // Defer layout to avoid stack overflow from deep JS↔C++ interleaving
    // layout() will be called by the host before the next paint
    return child;
};

Element.prototype.removeChild = function(child) {
    var idx = this._children.indexOf(child);
    if (idx < 0) return child;
    this._children.splice(idx, 1);
    child._parentElement = null;
    if (child._nativeCreated) removeWidget(child._id);
    child._nativeCreated = false;
    return child;
};

Element.prototype.insertBefore = function(newChild, refChild) {
    if (!refChild) return this.appendChild(newChild);
    if (newChild._parentElement) newChild._parentElement.removeChild(newChild);
    var idx = this._children.indexOf(refChild);
    if (idx < 0) return this.appendChild(newChild);
    newChild._parentElement = this;
    this._children.splice(idx, 0, newChild);
    this._ensureNative();
    newChild._ensureNative();
    removeWidget(newChild._id);
    _reparentNative(newChild, this._id);
    if (newChild._textContent) setText(newChild._id, newChild._textContent);
    newChild.style._flushAll();
    newChild._reapplyStylesheets();
    layout();
    return newChild;
};

Element.prototype.replaceChild = function(newChild, oldChild) {
    var idx = this._children.indexOf(oldChild);
    if (idx < 0) return oldChild;
    this.insertBefore(newChild, oldChild);
    this.removeChild(oldChild);
    return oldChild;
};

Element.prototype.remove = function() {
    if (this._parentElement) this._parentElement.removeChild(this);
};

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
    }
    // pulp #2163 — font v2 Slice 2.5. The HTML `dir` attribute
    // (`<div dir="rtl">…</div>`) parallels the CSS `direction: rtl`
    // path (web-compat-style-decl.js → setDirection). Both routes
    // land on View::WritingDirection which Yoga consumes for
    // container child-order flip + the shaper consumes for bidi
    // base direction. Values: "ltr" | "rtl" | "auto" (CSS spec).
    else if (name === "dir" && typeof setDirection !== "undefined") {
        var dv = String(value).toLowerCase();
        if (dv === "rtl" || dv === "ltr" || dv === "auto") {
            setDirection(this._id, dv);
        }
    }
};

Element.prototype.getAttribute = function(name) {
    if (name === "id") return this.id;
    if (name === "class") return this.className;
    return this._attributes[name] !== undefined ? this._attributes[name] : null;
};

Element.prototype.removeAttribute = function(name) {
    delete this._attributes[name];
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
    get: function() { return typeof document !== "undefined" ? document : null; }
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

// ── P1: closest(), matches(), contains(), querySelector on elements ──────

Element.prototype.closest = function(selector) {
    var parsed = _parseSelector(selector);
    var el = this;
    while (el) {
        if (_matchesSelector(el, parsed)) return el;
        el = el._parentElement;
    }
    return null;
};

Element.prototype.matches = function(selector) {
    var parsed = _parseSelector(selector);
    return _matchesSelector(this, parsed);
};

Element.prototype.contains = function(other) {
    var el = other;
    while (el) {
        if (el === this) return true;
        el = el._parentElement;
    }
    return false;
};

Element.prototype.querySelector = function(selector) {
    return _querySelector(this, selector);
};

Element.prototype.querySelectorAll = function(selector) {
    return _querySelectorAll(this, selector);
};

// ── P1: innerHTML (simple HTML parser for common patterns) ───────────────

Object.defineProperty(Element.prototype, "innerHTML", {
    get: function() {
        var out = "";
        for (var i = 0; i < this._children.length; i++) {
            out += _serializeElement(this._children[i]);
        }
        return out;
    },
    set: function(html) {
        // Remove existing children
        while (this._children.length > 0) {
            this.removeChild(this._children[0]);
        }
        if (!html) return;
        // Parse and append
        var nodes = _parseHTML(html);
        for (var i = 0; i < nodes.length; i++) {
            this.appendChild(nodes[i]);
        }
    }
});

Object.defineProperty(Element.prototype, "outerHTML", {
    get: function() { return _serializeElement(this); }
});

function _serializeElement(el) {
    var tag = el.tagName.toLowerCase();
    var attrs = "";
    if (el._attributes["id"]) attrs += ' id="' + el._attributes["id"] + '"';
    if (el._className) attrs += ' class="' + el._className + '"';
    var inner = el._textContent || "";
    for (var i = 0; i < el._children.length; i++) {
        inner += _serializeElement(el._children[i]);
    }
    return "<" + tag + attrs + ">" + inner + "</" + tag + ">";
}

function _parseHTML(html) {
    var nodes = [];
    var re = /<(\w+)([^>]*)>([\s\S]*?)<\/\1>|<(\w+)([^>]*)\s*\/?>|([^<]+)/g;
    var m;
    while ((m = re.exec(html)) !== null) {
        if (m[1]) {
            // Opening + closing tag: <tag attrs>content</tag>
            var el = document.createElement(m[1]);
            _parseAttrs(el, m[2]);
            if (m[3]) {
                // Check for nested tags
                if (m[3].indexOf("<") >= 0) {
                    var children = _parseHTML(m[3]);
                    for (var i = 0; i < children.length; i++) el.appendChild(children[i]);
                } else {
                    el.textContent = m[3];
                }
            }
            nodes.push(el);
        } else if (m[4]) {
            // Self-closing tag: <tag attrs/>
            var el2 = document.createElement(m[4]);
            _parseAttrs(el2, m[5]);
            nodes.push(el2);
        } else if (m[6] && m[6].trim()) {
            // Text node
            var tn = document.createElement("span");
            tn.textContent = m[6];
            nodes.push(tn);
        }
    }
    return nodes;
}

function _parseAttrs(el, attrStr) {
    if (!attrStr) return;
    var re = /(\w[\w-]*)(?:="([^"]*)")?/g;
    var m;
    while ((m = re.exec(attrStr)) !== null) {
        var name = m[1], value = m[2] || "";
        if (name === "class") el.className = value;
        else if (name === "id") el.id = value;
        else el.setAttribute(name, value);
    }
}

// ── P2: Modern DOM insertion methods ─────────────────────────────────────

Element.prototype.append = function() {
    for (var i = 0; i < arguments.length; i++) {
        var arg = arguments[i];
        if (typeof arg === "string") {
            var tn = document.createElement("span");
            tn.textContent = arg;
            this.appendChild(tn);
        } else {
            this.appendChild(arg);
        }
    }
};

Element.prototype.prepend = function() {
    var first = this._children[0] || null;
    for (var i = 0; i < arguments.length; i++) {
        var arg = arguments[i];
        if (typeof arg === "string") {
            var tn = document.createElement("span");
            tn.textContent = arg;
            this.insertBefore(tn, first);
        } else {
            this.insertBefore(arg, first);
        }
    }
};

Element.prototype.before = function() {
    if (!this._parentElement) return;
    for (var i = 0; i < arguments.length; i++) {
        var arg = arguments[i];
        if (typeof arg === "string") {
            var tn = document.createElement("span");
            tn.textContent = arg;
            this._parentElement.insertBefore(tn, this);
        } else {
            this._parentElement.insertBefore(arg, this);
        }
    }
};

Element.prototype.after = function() {
    if (!this._parentElement) return;
    var next = this.nextSibling;
    for (var i = 0; i < arguments.length; i++) {
        var arg = arguments[i];
        if (typeof arg === "string") {
            var tn = document.createElement("span");
            tn.textContent = arg;
            this._parentElement.insertBefore(tn, next);
        } else {
            this._parentElement.insertBefore(arg, next);
        }
    }
};

Element.prototype.replaceWith = function() {
    if (!this._parentElement) return;
    var parent = this._parentElement;
    var next = this.nextSibling;
    parent.removeChild(this);
    for (var i = 0; i < arguments.length; i++) {
        var arg = arguments[i];
        if (typeof arg === "string") {
            var tn = document.createElement("span");
            tn.textContent = arg;
            parent.insertBefore(tn, next);
        } else {
            parent.insertBefore(arg, next);
        }
    }
};

// ── P2: focus() / blur() ─────────────────────────────────────────────────

Element.prototype.focus = function() {
    if (this._nativeCreated) {
        // Dispatch focus event
        var evt = _makeEvent("focus", this);
        this.dispatchEvent(evt);
    }
};

Element.prototype.blur = function() {
    if (this._nativeCreated) {
        var evt = _makeEvent("blur", this);
        this.dispatchEvent(evt);
    }
};

// ── Web Animations API: element.animate(keyframes, options) ──────────────

function Animation(el, keyframes, duration, easing, fill) {
    this._el = el;
    this._keyframes = keyframes;
    this._duration = duration || 300;
    this._easing = easing || "ease";
    this._fill = fill || "none";
    this._startTime = null;
    this._finished = false;
    this._cancelled = false;
    this._onfinish = null;
    this.playState = "idle";

    // Create a promise-like finished property
    var self = this;
    this.finished = { then: function(fn) { self._onfinish = fn; } };
}

Animation.prototype.play = function() {
    if (this._cancelled) return;
    this.playState = "running";
    var self = this;
    var el = this._el;
    var keyframes = this._keyframes;
    var duration = this._duration;
    var startProps = {};

    // Capture start values
    for (var k in keyframes[0]) {
        startProps[k] = el.style[k] || keyframes[0][k];
    }

    var startTime = performance.now();
    function tick() {
        if (self._cancelled) return;
        var elapsed = performance.now() - startTime;
        var t = Math.min(elapsed / duration, 1);

        // Apply easing (simple ease-out for now)
        if (self._easing === "ease-out") t = 1 - Math.pow(1 - t, 3);
        else if (self._easing === "ease-in") t = Math.pow(t, 3);
        else if (self._easing === "ease-in-out") t = t < 0.5 ? 4*t*t*t : 1-Math.pow(-2*t+2,3)/2;

        // Interpolate between first and last keyframe
        var from = keyframes[0];
        var to = keyframes[keyframes.length - 1];
        for (var prop in to) {
            var fromVal = from[prop] || startProps[prop] || "";
            var toVal = to[prop];
            // Try numeric interpolation for px values
            var fromNum = parseFloat(fromVal);
            var toNum = parseFloat(toVal);
            if (!isNaN(fromNum) && !isNaN(toNum)) {
                var interp = fromNum + (toNum - fromNum) * t;
                var unit = String(toVal).replace(/[\d.-]/g, "") || "px";
                el.style[prop] = interp + unit;
            } else if (t >= 1) {
                el.style[prop] = toVal;
            }
        }

        if (t < 1) {
            window.requestAnimationFrame(tick);
        } else {
            self._finished = true;
            self.playState = "finished";
            if (self._fill === "none") {
                // Revert to start values
                for (var p in startProps) el.style[p] = startProps[p];
            }
            if (self._onfinish) self._onfinish();
            el.dispatchEvent(_makeEvent("animationend", el));
        }
    }

    window.requestAnimationFrame(tick);
};

Animation.prototype.cancel = function() {
    this._cancelled = true;
    this.playState = "idle";
};

Animation.prototype.pause = function() { this.playState = "paused"; };
Animation.prototype.finish = function() { this._finished = true; this.playState = "finished"; };

Element.prototype.animate = function(keyframes, options) {
    var duration = typeof options === "number" ? options : (options && options.duration || 300);
    var easing = (options && options.easing) || "ease";
    var fill = (options && options.fill) || "none";
    var anim = new Animation(this, keyframes, duration, easing, fill);
    anim.play();
    return anim;
};

Element.prototype._registerNativeEvent = function(type) {
    var id = this._id;
    var self = this;
    if (type === "click" || type === "mousedown" || type === "mouseup") {
        registerClick(id);
        on(id, "click", function() {
            var evt = _makeEvent("click", self);
            self.dispatchEvent(evt);
        });
    } else if (type === "mouseenter" || type === "mouseleave") {
        registerHover(id);
        on(id, "mouseenter", function() {
            var evt = _makeEvent("mouseenter", self);
            evt._noBubble = true;
            _fireListeners(self, evt);
        });
        on(id, "mouseleave", function() {
            var evt = _makeEvent("mouseleave", self);
            evt._noBubble = true;
            _fireListeners(self, evt);
        });
    } else if (type === "input" || type === "change") {
        on(id, "change", function(val) {
            self._value = val;
            var evt = _makeEvent("input", self);
            self.dispatchEvent(evt);
            var evt2 = _makeEvent("change", self);
            self.dispatchEvent(evt2);
        });
    } else if (type === "dblclick") {
        registerClick(id);
        on(id, "dblclick", function() {
            self.dispatchEvent(_makeEvent("dblclick", self));
        });
    } else if (type === "wheel") {
        if (typeof registerWheel === "function") registerWheel(id);
        on(id, "wheel", function(dx, dy) {
            self.dispatchEvent(_makeEvent("wheel", self, { deltaX: dx || 0, deltaY: dy || 0 }));
        });
    } else if (type === "scroll") {
        on(id, "scroll", function() {
            self.dispatchEvent(_makeEvent("scroll", self));
        });
    } else if (type === "contextmenu") {
        if (typeof registerContextMenu === "function") {
            registerContextMenu(id, "__dispatch__('" + id + "', 'contextmenu', 0)");
        }
        on(id, "contextmenu", function() {
            self.dispatchEvent(_makeEvent("contextmenu", self));
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

function _makeEvent(type, target, data) {
    var d = data || {};
    return {
        type: type,
        target: target,
        currentTarget: null,
        clientX: d.clientX || 0,
        clientY: d.clientY || 0,
        offsetX: d.offsetX || 0,
        offsetY: d.offsetY || 0,
        button: d.button || 0,
        key: d.key || "", code: d.code || "",
        ctrlKey: !!d.ctrlKey, shiftKey: !!d.shiftKey,
        altKey: !!d.altKey, metaKey: !!d.metaKey,
        pointerId: d.pointerId || 0,
        pointerType: d.pointerType || "mouse",
        isPrimary: d.isPrimary !== undefined ? d.isPrimary : true,
        pressure: d.pressure !== undefined ? d.pressure : 0.5,
        altitudeAngle: d.altitudeAngle || 0,
        azimuthAngle: d.azimuthAngle || 0,
        scale: d.scale !== undefined ? d.scale : 1,
        rotation: d.rotation || 0,
        _coalesced: d._coalesced || null,
        _predicted: d._predicted || null,
        getCoalescedEvents: function() { return this._coalesced || [this]; },
        getPredictedEvents: function() { return this._predicted || []; },
        detail: d.detail || null,
        deltaX: d.deltaX || 0, deltaY: d.deltaY || 0, deltaMode: d.deltaMode || 0,
        _stopped: false,
        _stoppedImmediate: false,
        _defaultPrevented: false,
        _noBubble: false,
        stopPropagation: function() { this._stopped = true; },
        stopImmediatePropagation: function() { this._stopped = true; this._stoppedImmediate = true; },
        preventDefault: function() { this._defaultPrevented = true; }
    };
}

// CustomEvent constructor
function CustomEvent(type, opts) {
    var o = opts || {};
    var evt = _makeEvent(type, null, { detail: o.detail });
    evt.bubbles = o.bubbles !== undefined ? o.bubbles : false;
    evt.cancelable = o.cancelable !== undefined ? o.cancelable : false;
    return evt;
}

function _fireListeners(el, event) {
    var id = el._id;
    var listeners = __eventListeners__[id] && __eventListeners__[id][event.type];
    if (!listeners) return;
    event.currentTarget = el;
    for (var i = 0; i < listeners.length; i++) {
        listeners[i].fn.call(el, event);
        if (event._stoppedImmediate || event._stopped) break;
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

    // Recursively reparent children
    for (var i = 0; i < child._children.length; i++) {
        var c = child._children[i];
        if (c._nativeCreated) removeWidget(c._id);
        _reparentNative(c, id);
        if (c._textContent) setText(c._id, c._textContent);
        c.style._flushAll();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration
// ═══════════════════════════════════════════════════════════════════════════════

function CSSStyleDeclaration(el) {
    this._el = el;
    this._props = {};
}

// Flush all stored properties to the bridge
CSSStyleDeclaration.prototype._flushAll = function() {
    for (var key in this._props) {
        this._applyProperty(key, this._props[key]);
    }
};

// Apply a single CSS property to the bridge
CSSStyleDeclaration.prototype._applyProperty = function(key, value) {
    var id = this._el._id;
    if (!this._el._nativeCreated) return;

    var resolved = _resolveVar(String(value));

    switch (key) {
        // Display / flex direction
        case "display":
            if (resolved === "none") { setVisible(id, false); }
            // pulp #1420 — inline-block ≡ block, inline-flex ≡ flex in pulp's
            // non-text-flowing layout (matches RN + CSS formatting-context
            // semantics where there is no inline flow). 4 of these were
            // silently dropped by Spectr today.
            else if (resolved === "flex" || resolved === "block" ||
                     resolved === "inline-block" || resolved === "inline-flex") {
                setVisible(id, true);
            }
            else if (resolved === "grid") { /* grid mode set via gridTemplateColumns */ }
            break;
        case "flexDirection":
            // pulp #1434 (rn batch B) — forward all four CSS values
            // verbatim so the bridge can route to YGFlexDirectionRow /
            // RowReverse / Column / ColumnReverse.
            setFlex(id, "direction",
                resolved === "row" ? "row" :
                resolved === "row-reverse" ? "row-reverse" :
                resolved === "column-reverse" ? "column-reverse" :
                "col");
            break;
        case "flexWrap":
            // pulp #1434 Triage #14 — forward keyword verbatim so the
            // bridge can route wrap-reverse through YGWrapWrapReverse.
            setFlex(id, "flex_wrap", resolved);
            break;
        case "flexGrow":
            setFlex(id, "flex_grow", parseFloat(resolved) || 0);
            break;
        case "flexShrink":
            setFlex(id, "flex_shrink", parseFloat(resolved) || 0);
            break;
        case "flexBasis":
            var fb = parseCSSLength(resolved);
            if (fb) setFlex(id, "flex_basis", fb.value);
            break;
        case "flex": {
            // Shorthand: flex: <grow> [<shrink>] [<basis>]
            var parts = resolved.split(/\s+/);
            setFlex(id, "flex_grow", parseFloat(parts[0]) || 0);
            if (parts[1]) setFlex(id, "flex_shrink", parseFloat(parts[1]) || 0);
            if (parts[2]) { var b = parseCSSLength(parts[2]); if (b) setFlex(id, "flex_basis", b.value); }
            break;
        }
        case "justifyContent":
            setFlex(id, "justify_content", _cssToFlex(resolved));
            break;
        case "alignItems":
            setFlex(id, "align_items", _cssToFlex(resolved));
            break;
        case "alignSelf":
            setFlex(id, "align_self", _cssToFlex(resolved));
            break;
        case "order":
            setFlex(id, "order", parseInt(resolved) || 0);
            break;
        case "gap": {
            var g = parseCSSLength(resolved);
            if (g) setFlex(id, "gap", g.value);
            break;
        }
        case "rowGap": {
            var rg = parseCSSLength(resolved);
            if (rg) setFlex(id, "row_gap", rg.value);
            break;
        }
        case "columnGap": {
            var cg = parseCSSLength(resolved);
            if (cg) setFlex(id, "column_gap", cg.value);
            break;
        }

        // Dimensions — pulp #1423 forwards percent values verbatim so
        // the bridge can route to Yoga's percent API.
        case "width": {
            var w = parseCSSLength(resolved);
            if (!w) break;
            if (w.unit === "%") setFlex(id, "width", w.value + "%");
            else setFlex(id, "width", w.value);
            break;
        }
        case "height": {
            var h = parseCSSLength(resolved);
            if (!h) break;
            if (h.unit === "%") setFlex(id, "height", h.value + "%");
            else setFlex(id, "height", h.value);
            break;
        }
        case "minWidth": {
            var mw = parseCSSLength(resolved);
            if (mw) setFlex(id, "min_width", mw.value);
            break;
        }
        case "minHeight": {
            var mh = parseCSSLength(resolved);
            if (mh) setFlex(id, "min_height", mh.value);
            break;
        }
        case "maxWidth": {
            var xw = parseCSSLength(resolved);
            if (xw) setFlex(id, "max_width", xw.value);
            break;
        }
        case "maxHeight": {
            var xh = parseCSSLength(resolved);
            if (xh) setFlex(id, "max_height", xh.value);
            break;
        }

        // Margin (individual)
        case "marginTop": {
            var mt = parseCSSLength(resolved);
            if (mt) setFlex(id, "margin_top", mt.value);
            break;
        }
        case "marginRight": {
            var mr = parseCSSLength(resolved);
            if (mr) setFlex(id, "margin_right", mr.value);
            break;
        }
        case "marginBottom": {
            var mb = parseCSSLength(resolved);
            if (mb) setFlex(id, "margin_bottom", mb.value);
            break;
        }
        case "marginLeft": {
            var ml = parseCSSLength(resolved);
            if (ml) setFlex(id, "margin_left", ml.value);
            break;
        }
        // Margin shorthand
        case "margin": {
            var ms = expandShorthand(resolved);
            setFlex(id, "margin_top", ms[0]);
            setFlex(id, "margin_right", ms[1]);
            setFlex(id, "margin_bottom", ms[2]);
            setFlex(id, "margin_left", ms[3]);
            break;
        }

        // Padding (individual)
        case "paddingTop": {
            var pt = parseCSSLength(resolved);
            if (pt) setFlex(id, "padding_top", pt.value);
            break;
        }
        case "paddingRight": {
            var pr = parseCSSLength(resolved);
            if (pr) setFlex(id, "padding_right", pr.value);
            break;
        }
        case "paddingBottom": {
            var pb = parseCSSLength(resolved);
            if (pb) setFlex(id, "padding_bottom", pb.value);
            break;
        }
        case "paddingLeft": {
            var pl = parseCSSLength(resolved);
            if (pl) setFlex(id, "padding_left", pl.value);
            break;
        }
        // Padding shorthand
        case "padding": {
            var ps = expandShorthand(resolved);
            setFlex(id, "padding_top", ps[0]);
            setFlex(id, "padding_right", ps[1]);
            setFlex(id, "padding_bottom", ps[2]);
            setFlex(id, "padding_left", ps[3]);
            break;
        }

        // Colors
        case "backgroundColor": {
            var bgColor = parseCSSColor(resolved);
            if (bgColor) setBackground(id, bgColor);
            break;
        }
        case "color": {
            var txtColor = parseCSSColor(resolved);
            if (txtColor) setTextColor(id, txtColor);
            break;
        }

        // Typography
        case "fontSize": {
            var fs = parseCSSLength(resolved);
            if (fs) setFontSize(id, fs.value);
            break;
        }
        case "fontWeight":
            setFontWeight(id, parseInt(resolved) || 400);
            break;
        case "fontStyle":
            setFontStyle(id, resolved);
            break;
        case "letterSpacing": {
            var ls = parseCSSLength(resolved);
            if (ls) setLetterSpacing(id, ls.value);
            break;
        }
        case "lineHeight": {
            var lh = parseCSSLength(resolved);
            if (lh) setLineHeight(id, lh.value);
            break;
        }
        case "textAlign":
            setTextAlign(id, resolved);
            break;
        case "textTransform":
            setTextTransform(id, resolved);
            break;
        case "textDecoration":
            setTextDecoration(id, resolved);
            break;
        case "textOverflow":
            setTextOverflow(id, resolved);
            break;

        // Border
        case "borderRadius": {
            var br = parseCSSLength(resolved);
            if (br) setBorder(id, "", 0, br.value);
            break;
        }
        case "border": {
            // "1px solid #333"
            var bp = resolved.match(/([\d.]+)px\s+\w+\s+(.+)/);
            if (bp) {
                var bc = parseCSSColor(bp[2].trim());
                setBorder(id, bc || bp[2].trim(), parseFloat(bp[1]), 0);
            }
            break;
        }
        case "borderColor": {
            var bcc = parseCSSColor(resolved);
            if (bcc) setBorder(id, bcc, 1, 0);
            break;
        }
        case "borderWidth": {
            var bw = parseCSSLength(resolved);
            if (bw) setBorder(id, "", bw.value, 0);
            break;
        }

        // Opacity
        case "opacity":
            setOpacity(id, parseFloat(resolved) || 0);
            break;

        // Overflow
        case "overflow":
            setOverflow(id, resolved);
            break;

        // Cursor
        case "cursor":
            setCursor(id, resolved);
            break;

        // Transform
        case "transform": {
            var transforms = parseTransform(resolved);
            for (var i = 0; i < transforms.length; i++) {
                var t = transforms[i];
                if (t.fn === "scale") setScale(id, t.args[0] || 1);
                else if (t.fn === "rotate") setRotation(id, t.args[0] || 0);
                else if (t.fn === "translate") setTranslate(id, t.args[0] || 0, t.args[1] || 0);
                else if (t.fn === "translateX") setTranslate(id, t.args[0] || 0, 0);
                else if (t.fn === "translateY") setTranslate(id, 0, t.args[0] || 0);
            }
            break;
        }
        case "transformOrigin": {
            // "center", "left top", "50% 50%", "10px 20px"
            var ox = 0.5, oy = 0.5;
            var op = resolved.split(/\s+/);
            function _parseOrigin(v) {
                if (v === "center") return 0.5;
                if (v === "left" || v === "top") return 0;
                if (v === "right" || v === "bottom") return 1;
                var l = parseCSSLength(v);
                if (l && l.unit === "%") return l.value / 100;
                return 0.5;
            }
            ox = _parseOrigin(op[0] || "center");
            oy = _parseOrigin(op[1] || op[0] || "center");
            setTransformOrigin(id, ox, oy);
            break;
        }

        // Transition
        case "transition": {
            var tr = parseTransition(resolved);
            setTransitionDuration(id, tr.duration);
            break;
        }
        case "transitionDuration": {
            var td = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) td /= 1000;
            setTransitionDuration(id, td);
            break;
        }

        // Position
        case "position":
            setPosition(id, resolved);
            break;
        // pulp #1434 batch 6 — top/right/bottom/left forward percent
        // values verbatim so the bridge can route to Yoga's percent API.
        case "top": {
            var tv = parseCSSLength(resolved); if (!tv) break;
            if (tv.unit === "%") setTop(id, tv.value + "%"); else setTop(id, tv.value);
            break;
        }
        case "right": {
            var rv = parseCSSLength(resolved); if (!rv) break;
            if (rv.unit === "%") setRight(id, rv.value + "%"); else setRight(id, rv.value);
            break;
        }
        case "bottom": {
            var bv = parseCSSLength(resolved); if (!bv) break;
            if (bv.unit === "%") setBottom(id, bv.value + "%"); else setBottom(id, bv.value);
            break;
        }
        case "left": {
            var lv = parseCSSLength(resolved); if (!lv) break;
            if (lv.unit === "%") setLeft(id, lv.value + "%"); else setLeft(id, lv.value);
            break;
        }

        // z-index
        case "zIndex":
            setZIndex(id, parseInt(resolved) || 0);
            break;

        // Box shadow: "2px 4px 8px rgba(0,0,0,0.3)" or "inset 2px 4px 8px ..." (issue-925)
        case "boxShadow": {
            if (resolved === "none" || resolved === "" || resolved == null) {
                if (typeof clearBoxShadow === "function") clearBoxShadow(id);
                break;
            }
            var work = String(resolved).trim();
            var inset = false;
            if (/^inset\s+/i.test(work)) {
                inset = true;
                work = work.replace(/^inset\s+/i, "");
            } else if (/\s+inset\s*$/i.test(work)) {
                inset = true;
                work = work.replace(/\s+inset\s*$/i, "");
            }
            var sm = work.match(/(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px(?:\s+(-?[\d.]+)px)?\s+(.*)/);
            if (sm) {
                var sc = parseCSSColor(sm[5].trim());
                setBoxShadow(id, parseFloat(sm[1]), parseFloat(sm[2]),
                            parseFloat(sm[3]), parseFloat(sm[4] || 0),
                            sc || sm[5].trim(), inset);
            }
            break;
        }

        // Filter
        case "filter":
            setFilter(id, resolved);
            break;

        // Background gradient
        case "backgroundImage":
        case "background": {
            if (resolved.indexOf("gradient") >= 0) {
                setBackgroundGradient(id, resolved);
            } else {
                var bgc2 = parseCSSColor(resolved);
                if (bgc2) setBackground(id, bgc2);
            }
            break;
        }

        // Grid
        case "gridTemplateColumns":
            setGrid(id, "template_columns", resolved);
            break;
        case "gridTemplateRows":
            setGrid(id, "template_rows", resolved);
            break;
        case "gridColumn": {
            var gc = resolved.split("/").map(function(s) { return parseInt(s.trim()); });
            if (gc[0]) setGrid(id, "column_start", gc[0]);
            if (gc[1]) setGrid(id, "column_end", gc[1]);
            break;
        }
        case "gridRow": {
            var gr = resolved.split("/").map(function(s) { return parseInt(s.trim()); });
            if (gr[0]) setGrid(id, "row_start", gr[0]);
            if (gr[1]) setGrid(id, "row_end", gr[1]);
            break;
        }
    }
};

// Convert CSS flex alignment names to Pulp bridge names
function _cssToFlex(v) {
    if (v === "flex-start") return "start";
    if (v === "flex-end") return "end";
    if (v === "space-between") return "space-between";
    if (v === "space-around") return "space-around";
    if (v === "space-evenly") return "space-evenly";
    return v; // center, stretch pass through
}

// Define style property getters/setters via Proxy-like approach
// Since QuickJS supports Proxy, use it for the style object
// But for safety and compatibility, we use defineProperty on the prototype
var __cssProperties__ = [
    "display", "flexDirection", "flexWrap", "flexGrow", "flexShrink", "flexBasis", "flex",
    "justifyContent", "alignItems", "alignSelf", "order",
    "gap", "rowGap", "columnGap",
    "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
    "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
    "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "backgroundColor", "color",
    "fontSize", "fontWeight", "fontStyle", "letterSpacing", "lineHeight",
    "textAlign", "textTransform", "textDecoration", "textOverflow",
    "border", "borderColor", "borderWidth", "borderRadius",
    "opacity", "overflow", "cursor",
    "transform", "transformOrigin",
    "transition", "transitionDuration",
    "position", "top", "right", "bottom", "left", "zIndex",
    "boxShadow", "filter", "background", "backgroundImage",
    "gridTemplateColumns", "gridTemplateRows", "gridColumn", "gridRow"
];

(function() {
    for (var i = 0; i < __cssProperties__.length; i++) {
        (function(prop) {
            Object.defineProperty(CSSStyleDeclaration.prototype, prop, {
                get: function() { return this._props[prop] || ""; },
                set: function(v) {
                    this._props[prop] = v;
                    this._applyProperty(prop, v);
                },
                enumerable: true, configurable: true
            });
        })(__cssProperties__[i]);
    }
})();

// setProperty / getPropertyValue for CSS variable support
CSSStyleDeclaration.prototype.setProperty = function(name, value) {
    // --custom-property -> set as theme token
    if (name.indexOf("--") === 0) {
        var tokenName = name.slice(2);
        // pulp #1918 (Codex review) — clear every non-active slot
        // first so a var() reassignment that changes the value type
        // (string ↔ length ↔ color) doesn't leave a stale token in
        // one of the other slots. See the matching fix and rationale
        // in web-compat-style-decl.js.
        if (typeof setStringToken === 'function') setStringToken(tokenName, "");
        if (typeof setMotionToken === 'function') setMotionToken(tokenName, 0);
        var parsed = parseCSSLength(value);
        if (parsed) {
            setMotionToken(tokenName, parsed.value);
        } else {
            // Color token? Store as a theme color override
            var color = parseCSSColor(value);
            if (color) {
                // Use applyTokenDiff for color tokens
                applyTokenDiff('{"colors":{"' + tokenName + '":"' + color + '"}}');
            } else if (typeof setStringToken === 'function') {
                // pulp #1899 (gap #3) — string-valued custom property
                // (font family / arbitrary string). Mirrors
                // web-compat-style-decl.js.
                setStringToken(tokenName, String(value));
            }
        }
    } else {
        // Convert CSS property name to camelCase
        var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
        this[camel] = value;
    }
};

CSSStyleDeclaration.prototype.getPropertyValue = function(name) {
    if (name.indexOf("--") === 0) {
        var tokenName = name.slice(2);
        // pulp #1899 (gap #3) — string-token round-trip; mirrors
        // web-compat-style-decl.js.
        if (typeof getStringToken === 'function') {
            var s = getStringToken(tokenName);
            if (s) return s;
        }
        return String(getMotionToken(tokenName));
    }
    var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
    return this._props[camel] || "";
};

CSSStyleDeclaration.prototype.removeProperty = function(name) {
    var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
    var old = this._props[camel] || "";
    delete this._props[camel];
    return old;
};

// ═══════════════════════════════════════════════════════════════════════════════
// StyleSheet
// ═══════════════════════════════════════════════════════════════════════════════

function StyleSheet(rules) {
    this._rules = rules || {};
    this._attached = false;
    this._parsedRules = [];

    // Parse rules into structured form
    for (var selector in this._rules) {
        this._parsedRules.push({
            selector: selector,
            properties: this._rules[selector],
            parsed: _parseSelector(selector)
        });
    }
}

StyleSheet.prototype.attach = function() {
    if (this._attached) return;
    this._attached = true;
    __stylesheets__.push(this);
    // Apply to all existing elements
    for (var id in __elements__) {
        if (id[0] !== "#") { // Skip getElementById entries
            this._applyTo(__elements__[id]);
        }
    }
};

StyleSheet.prototype.detach = function() {
    var idx = __stylesheets__.indexOf(this);
    if (idx >= 0) __stylesheets__.splice(idx, 1);
    this._attached = false;
};

StyleSheet.prototype._applyTo = function(el) {
    for (var i = 0; i < this._parsedRules.length; i++) {
        var rule = this._parsedRules[i];
        var parsed = rule.parsed;

        // Handle pseudo-classes separately
        if (parsed.pseudo === "hover") {
            if (_matchesSelector(el, parsed)) {
                _setupPseudoHover(el, rule.properties);
            }
        } else if (parsed.pseudo === "focus") {
            if (_matchesSelector(el, parsed)) {
                _setupPseudoFocus(el, rule.properties);
            }
        } else if (parsed.pseudo === "active") {
            if (_matchesSelector(el, parsed)) {
                _setupPseudoActive(el, rule.properties);
            }
        } else if (parsed.pseudo === "disabled") {
            if (_matchesSelector(el, parsed) && el._disabled) {
                _applyStyles(el, rule.properties);
            }
        } else {
            if (_matchesSelector(el, parsed)) {
                _applyStyles(el, rule.properties);
            }
        }
    }
};

function _applyStyles(el, props) {
    for (var k in props) {
        el.style[k] = props[k];
    }
}

function _setupPseudoHover(el, props) {
    if (el._hoverSetup) return;
    el._hoverSetup = true;
    var savedProps = {};

    el.addEventListener("mouseenter", function() {
        // Save current values
        for (var k in props) savedProps[k] = el.style[k];
        _applyStyles(el, props);
    });

    el.addEventListener("mouseleave", function() {
        // Restore
        for (var k in savedProps) el.style[k] = savedProps[k];
    });
}

function _setupPseudoFocus(el, props) {
    if (el._focusSetup) return;
    el._focusSetup = true;
    var savedProps = {};

    el.addEventListener("focus", function() {
        for (var k in props) savedProps[k] = el.style[k];
        _applyStyles(el, props);
    });

    el.addEventListener("blur", function() {
        for (var k in savedProps) el.style[k] = savedProps[k];
    });
}

function _setupPseudoActive(el, props) {
    if (el._activeSetup) return;
    el._activeSetup = true;
    var savedProps = {};

    el.addEventListener("mousedown", function() {
        for (var k in props) savedProps[k] = el.style[k];
        _applyStyles(el, props);
    });

    el.addEventListener("mouseup", function() {
        for (var k in savedProps) el.style[k] = savedProps[k];
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// Selector parsing and matching
// ═══════════════════════════════════════════════════════════════════════════════

function _parseSelector(str) {
    var result = { tag: null, id: null, classes: [], pseudo: null, pseudoArg: null,
                   notSelector: null, parent: null, direct: false, attrSelectors: [] };

    // Extract attribute selectors [attr], [attr="val"], [attr~="val"], [attr^="val"]
    str = str.replace(/\[([^\]]+)\]/g, function(_, inner) {
        var m = inner.match(/^([\w-]+)(?:([~|^$*]?)=["']?([^"'\]]*)["']?)?$/);
        if (m) result.attrSelectors.push({ name: m[1], op: m[2] || "", value: m[3] || "" });
        return "";
    });

    // Handle :not(selector) — extract inner selector
    var notMatch = str.match(/:not\(([^)]+)\)/);
    if (notMatch) {
        result.notSelector = _parseSelector(notMatch[1]);
        str = str.replace(/:not\([^)]+\)/, "");
    }

    // Split pseudo-class (but not :not which was already handled)
    var pseudoIdx = str.indexOf(":");
    var mainPart = str;
    if (pseudoIdx >= 0) {
        var pseudoStr = str.slice(pseudoIdx + 1);
        mainPart = str.slice(0, pseudoIdx);
        // Parse pseudo with optional argument: nth-child(2n+1)
        var pseudoArgMatch = pseudoStr.match(/^([\w-]+)\(([^)]*)\)/);
        if (pseudoArgMatch) {
            result.pseudo = pseudoArgMatch[1];
            result.pseudoArg = pseudoArgMatch[2];
        } else {
            result.pseudo = pseudoStr;
        }
    }

    // Check for descendant/child combinators
    if (mainPart.indexOf(" > ") >= 0) {
        var cp = mainPart.split(" > ");
        result.parent = _parseSelector(cp.slice(0, -1).join(" > "));
        result.direct = true;
        mainPart = cp[cp.length - 1].trim();
    } else if (mainPart.indexOf(" ") >= 0) {
        var sp = mainPart.split(/\s+/);
        result.parent = _parseSelector(sp.slice(0, -1).join(" "));
        result.direct = false;
        mainPart = sp[sp.length - 1].trim();
    }

    // Parse tag, id, classes from main part
    var parts = mainPart.match(/^([a-zA-Z][\w-]*)?([#.][^#.]+)*/);
    if (parts && parts[0]) {
        var tokens = mainPart.match(/([#.][a-zA-Z][\w-]*)|^([a-zA-Z][\w-]*)/g);
        if (tokens) {
            for (var i = 0; i < tokens.length; i++) {
                var t = tokens[i];
                if (t[0] === "#") result.id = t.slice(1);
                else if (t[0] === ".") result.classes.push(t.slice(1));
                else result.tag = t.toLowerCase();
            }
        }
    }

    return result;
}

// Get the index of an element among its parent's children (0-based)
function _childIndex(el) {
    if (!el._parentElement) return 0;
    var siblings = el._parentElement._children;
    for (var i = 0; i < siblings.length; i++) {
        if (siblings[i] === el) return i;
    }
    return 0;
}

// Parse An+B syntax: "odd" -> {a:2,b:1}, "even" -> {a:2,b:0}, "3" -> {a:0,b:3}, "2n+1" -> {a:2,b:1}
function _parseAnB(str) {
    str = str.trim().toLowerCase();
    if (str === "odd") return { a: 2, b: 1 };
    if (str === "even") return { a: 2, b: 0 };
    var m = str.match(/^(-?\d*)n\s*([+-]\s*\d+)?$/);
    if (m) {
        var a = m[1] === "" || m[1] === "+" ? 1 : m[1] === "-" ? -1 : parseInt(m[1]);
        var b = m[2] ? parseInt(m[2].replace(/\s/g, "")) : 0;
        return { a: a, b: b };
    }
    var n = parseInt(str);
    if (!isNaN(n)) return { a: 0, b: n };
    return { a: 0, b: 0 };
}

function _matchesNthChild(index1Based, anb) {
    if (anb.a === 0) return index1Based === anb.b;
    if (anb.a > 0) return (index1Based - anb.b) >= 0 && (index1Based - anb.b) % anb.a === 0;
    // Negative a: matches indices <= b
    return (index1Based - anb.b) <= 0 && (index1Based - anb.b) % anb.a === 0;
}

function _matchesSelector(el, parsed) {
    // Match tag
    if (parsed.tag && el.tagName.toLowerCase() !== parsed.tag) return false;

    // Match id
    if (parsed.id && el.getAttribute("id") !== parsed.id) return false;

    // Match classes
    for (var i = 0; i < parsed.classes.length; i++) {
        if (!el.classList.contains(parsed.classes[i])) return false;
    }

    // Match :not(selector)
    if (parsed.notSelector && _matchesSelector(el, parsed.notSelector)) return false;

    // Match pseudo-classes
    if (parsed.pseudo) {
        var p = parsed.pseudo;
        if (p === "first-child") {
            if (!el._parentElement || el._parentElement._children[0] !== el) return false;
        } else if (p === "last-child") {
            if (!el._parentElement) return false;
            var ch = el._parentElement._children;
            if (ch[ch.length - 1] !== el) return false;
        } else if (p === "nth-child") {
            var idx = _childIndex(el) + 1; // 1-based
            var anb = _parseAnB(parsed.pseudoArg || "0");
            if (!_matchesNthChild(idx, anb)) return false;
        } else if (p === "nth-last-child") {
            if (!el._parentElement) return false;
            var siblings = el._parentElement._children;
            var ridx = siblings.length - _childIndex(el); // 1-based from end
            var anb2 = _parseAnB(parsed.pseudoArg || "0");
            if (!_matchesNthChild(ridx, anb2)) return false;
        } else if (p === "only-child") {
            if (!el._parentElement || el._parentElement._children.length !== 1) return false;
        } else if (p === "empty") {
            if (el._children.length > 0 || (el._textContent && el._textContent.length > 0)) return false;
        } else if (p === "checked") {
            if (!el._checked) return false;
        } else if (p === "disabled") {
            if (!el._disabled) return false;
        } else if (p === "first-of-type") {
            if (!el._parentElement) return false;
            var tag = el.tagName;
            var fot = el._parentElement._children;
            var foundFirst = false;
            for (var fi = 0; fi < fot.length; fi++) {
                if (fot[fi].tagName === tag) { foundFirst = (fot[fi] === el); break; }
            }
            if (!foundFirst) return false;
        } else if (p === "last-of-type") {
            if (!el._parentElement) return false;
            var tag2 = el.tagName;
            var lot = el._parentElement._children;
            var foundLast = false;
            for (var li = lot.length - 1; li >= 0; li--) {
                if (lot[li].tagName === tag2) { foundLast = (lot[li] === el); break; }
            }
            if (!foundLast) return false;
        } else if (p === "nth-of-type") {
            if (!el._parentElement) return false;
            var tag3 = el.tagName;
            var notSiblings = el._parentElement._children;
            var typeIdx = 0;
            for (var ni = 0; ni < notSiblings.length; ni++) {
                if (notSiblings[ni].tagName === tag3) typeIdx++;
                if (notSiblings[ni] === el) break;
            }
            var anb3 = _parseAnB(parsed.pseudoArg || "0");
            if (!_matchesNthChild(typeIdx, anb3)) return false;
        } else if (p === "hover" || p === "focus" || p === "active") {
            // These are handled by StyleSheet pseudo-class registration, not static matching
        }
    }

    // Match attribute selectors
    if (parsed.attrSelectors) {
        for (var ai = 0; ai < parsed.attrSelectors.length; ai++) {
            var as = parsed.attrSelectors[ai];
            var attrVal = el.getAttribute(as.name);
            if (as.op === "" && as.value === "") {
                // [attr] — attribute exists
                if (attrVal === null) return false;
            } else if (as.op === "" || as.op === undefined) {
                // [attr="val"] — exact match
                if (attrVal !== as.value) return false;
            } else if (as.op === "~") {
                // [attr~="val"] — whitespace-separated word
                if (!attrVal || (" " + attrVal + " ").indexOf(" " + as.value + " ") < 0) return false;
            } else if (as.op === "^") {
                // [attr^="val"] — starts with
                if (!attrVal || attrVal.indexOf(as.value) !== 0) return false;
            } else if (as.op === "$") {
                // [attr$="val"] — ends with
                if (!attrVal || attrVal.indexOf(as.value, attrVal.length - as.value.length) < 0) return false;
            } else if (as.op === "*") {
                // [attr*="val"] — contains
                if (!attrVal || attrVal.indexOf(as.value) < 0) return false;
            }
        }
    }

    // Match parent constraint
    if (parsed.parent) {
        if (parsed.direct) {
            if (!el._parentElement || !_matchesSelector(el._parentElement, parsed.parent)) return false;
        } else {
            var ancestor = el._parentElement;
            var found = false;
            while (ancestor) {
                if (_matchesSelector(ancestor, parsed.parent)) { found = true; break; }
                ancestor = ancestor._parentElement;
            }
            if (!found) return false;
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// querySelector / querySelectorAll
// ═══════════════════════════════════════════════════════════════════════════════

function _querySelector(root, selector) {
    var parsed = _parseSelector(selector);
    return _findMatch(root, parsed, false);
}

function _querySelectorAll(root, selector) {
    var parsed = _parseSelector(selector);
    return _findMatch(root, parsed, true);
}

function _findMatch(root, parsed, findAll) {
    var results = [];
    var queue = root._children.slice();

    while (queue.length > 0) {
        var el = queue.shift();
        if (_matchesSelector(el, parsed)) {
            if (!findAll) return el;
            results.push(el);
        }
        for (var i = 0; i < el._children.length; i++) {
            queue.push(el._children[i]);
        }
    }

    return findAll ? results : null;
}

// ═══════════════════════════════════════════════════════════════════════════════
// getComputedStyle
// ═══════════════════════════════════════════════════════════════════════════════

function getComputedStyle(el) {
    // Return a read-only object that reflects the element's current style
    // For properties set via style, return those; for layout properties, query bridge
    var obj = {};
    for (var k in el.style._props) {
        obj[k] = el.style._props[k];
    }

    // If native, query layout dimensions
    if (el._nativeCreated && typeof getLayoutRect === "function") {
        var rect = getLayoutRect(el._id);
        if (rect) {
            obj.width = rect.width + "px";
            obj.height = rect.height + "px";
        }
    }

    return obj;
}

// ═══════════════════════════════════════════════════════════════════════════════
// document object
// ═══════════════════════════════════════════════════════════════════════════════

var __bodyElement__ = new Element("div", "__root__");
__bodyElement__._nativeCreated = true; // root already exists
__elements__["__root__"] = __bodyElement__;

var __documentElement__ = new Element("html", "__docRoot__");
__documentElement__.style = new CSSStyleDeclaration(__documentElement__);
__documentElement__._nativeCreated = true;

var document = {
    body: __bodyElement__,
    documentElement: __documentElement__,

    createElement: function(tag) {
        var el = new Element(tag);
        __elements__[el._id] = el;
        return el;
    },

    getElementById: function(id) {
        return __elements__["#" + id] || null;
    },

    querySelector: function(selector) {
        return _querySelector(__bodyElement__, selector);
    },

    querySelectorAll: function(selector) {
        return _querySelectorAll(__bodyElement__, selector);
    },

    createTextNode: function(text) {
        var el = new Element("span");
        el._textContent = text;
        __elements__[el._id] = el;
        return el;
    },

    createDocumentFragment: function() {
        // Lightweight container — children transfer on appendChild
        var frag = new Element("div");
        frag._isFragment = true;
        __elements__[frag._id] = frag;
        return frag;
    }
};

// Override appendChild on fragments: transfer children instead of appending the fragment
var _origAppendChild = Element.prototype.appendChild;
Element.prototype.appendChild = function(child) {
    if (child && child._isFragment) {
        // Transfer all children from fragment to this element
        var kids = child._children.slice();
        for (var i = 0; i < kids.length; i++) {
            _origAppendChild.call(this, kids[i]);
        }
        return child;
    }
    return _origAppendChild.call(this, child);
};

// Option element support for <select>
// When a <select> element has children appended, sync items to native ComboBox
var _origSelectAppendChild = _origAppendChild;
(function() {
    var origAC = Element.prototype.appendChild;
    Element.prototype.appendChild = function(child) {
        var result = origAC.call(this, child);
        if (this.tagName === "SELECT" && this._nativeCreated) {
            _syncSelectOptions(this);
        }
        return result;
    };
})();

function _syncSelectOptions(selectEl) {
    var items = [];
    for (var i = 0; i < selectEl._children.length; i++) {
        var opt = selectEl._children[i];
        if (opt.tagName === "OPTION") {
            items.push(opt._textContent || opt.getAttribute("value") || "");
        }
    }
    if (items.length > 0 && typeof setItems === "function") {
        // Build choc value array
        var arr = [];
        for (var j = 0; j < items.length; j++) arr.push(items[j]);
        setItems(selectEl._id, arr);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// window object (minimal shim)
// ═══════════════════════════════════════════════════════════════════════════════

// Helper: get actual root view dimensions from native bridge
function _getRootDims() {
    if (typeof getRootSize === "function") {
        var s = getRootSize();
        return { w: s.width || 800, h: s.height || 600 };
    }
    return { w: 800, h: 600 };
}

var window = {
    document: document,
    getComputedStyle: getComputedStyle,

    // Dynamic dimensions — query native root view size
    get innerWidth() { return _getRootDims().w; },
    get innerHeight() { return _getRootDims().h; },
    devicePixelRatio: 2,

    requestAnimationFrame: function(fn) {
        if (typeof __requestFrame__ === "function") {
            if (typeof fn !== "function") return 0;
            var id = __frameNextId__++;
            __frameCallbacks__[id] = fn;
            return __requestFrame__(id);
        }
        return 0;
    },
    cancelAnimationFrame: function(id) {
        if (typeof __cancelFrame__ === "function") __cancelFrame__(id);
    },

    // matchMedia — responsive breakpoint queries
    matchMedia: function(query) {
        var matches = _matchMediaQuery(query);
        return {
            matches: matches,
            media: query,
            onchange: null,
            addEventListener: function(type, fn) { this.onchange = fn; },
            removeEventListener: function() { this.onchange = null; },
            addListener: function(fn) { this.onchange = fn; },    // deprecated but expected
            removeListener: function() { this.onchange = null; }   // deprecated but expected
        };
    },

    // Stubs for common window properties developers check for
    navigator: { userAgent: "Pulp/1.0", platform: "native" },
    location: { href: "", pathname: "", search: "", hash: "" },
    setTimeout: function(fn, ms) {
        // Approximate via requestAnimationFrame chain
        var frames = Math.max(1, Math.round((ms || 0) / 16));
        var count = 0;
        function tick() {
            if (++count >= frames) fn();
            else window.requestAnimationFrame(tick);
        }
        window.requestAnimationFrame(tick);
        return 0;
    },
    clearTimeout: function() {},
    setInterval: function(fn, ms) {
        var frames = Math.max(1, Math.round((ms || 16) / 16));
        var count = 0;
        function tick() {
            if (++count >= frames) { fn(); count = 0; }
            window.requestAnimationFrame(tick);
        }
        window.requestAnimationFrame(tick);
        return 0;
    },
    clearInterval: function() {},

    // window.addEventListener — global event listeners (keydown, resize, etc.)
    _listeners: {},
    addEventListener: function(type, fn, opts) {
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(fn);
    },
    removeEventListener: function(type, fn) {
        var list = this._listeners[type];
        if (!list) return;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === fn) list.splice(i, 1);
        }
    },
    dispatchEvent: function(event) {
        var list = this._listeners[event.type];
        if (!list) return;
        for (var i = 0; i < list.length; i++) list[i](event);
    },

    // CustomEvent constructor
    CustomEvent: CustomEvent,

    // window.onerror
    onerror: null
};

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 9: Runtime APIs
// ═══════════════════════════════════════════════════════════════════════════════

// performance.now() — high-resolution monotonic time
var performance = {
    now: function() {
        if (typeof __performanceNow__ === "function") return __performanceNow__();
        return Date.now();
    }
};

if (typeof globalThis.window === "undefined") {
    globalThis.window = window;
}

if (typeof globalThis.self === "undefined") {
    globalThis.self = globalThis.window;
}

if (typeof globalThis.requestAnimationFrame === "undefined") {
    globalThis.requestAnimationFrame = function(fn) {
        return globalThis.window.requestAnimationFrame(fn);
    };
}

if (typeof globalThis.cancelAnimationFrame === "undefined") {
    globalThis.cancelAnimationFrame = function(id) {
        return globalThis.window.cancelAnimationFrame(id);
    };
}

// navigator.clipboard — read/write clipboard
if (!window.navigator.clipboard) {
    window.navigator.clipboard = {
        readText: function() {
            if (typeof readClipboard === "function") return readClipboard();
            return "";
        },
        writeText: function(text) {
            if (typeof writeClipboard === "function") writeClipboard(text);
        }
    };
}

// localStorage — file-backed key-value store
var localStorage = {
    getItem: function(key) {
        if (typeof storageGetItem === "function") {
            var v = storageGetItem(key);
            return v || null;
        }
        return null;
    },
    setItem: function(key, value) {
        if (typeof storageSetItem === "function") storageSetItem(key, String(value));
    },
    removeItem: function(key) {
        if (typeof storageRemoveItem === "function") storageRemoveItem(key);
    },
    clear: function() { /* would need list of all keys */ },
    get length() { return 0; },
    key: function(i) { return null; }
};
var sessionStorage = localStorage; // alias

// Image constructor — for preloading images
function Image() {
    var self = this;
    self.src = "";
    self.width = 0;
    self.height = 0;
    self.onload = null;
    self.onerror = null;
    self.complete = false;

    Object.defineProperty(self, "src", {
        get: function() { return self._src || ""; },
        set: function(v) {
            self._src = v;
            if (v && typeof createImage === "function") {
                var id = __genId__();
                createImage(id, "");
                if (typeof setImageSource === "function") setImageSource(id, v);
                self.complete = true;
                if (self.onload) self.onload();
            }
        }
    });
}

// atob / btoa — base64 encoding/decoding
function btoa(str) {
    var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    var out = "";
    for (var i = 0; i < str.length; i += 3) {
        var a = str.charCodeAt(i);
        var b = i + 1 < str.length ? str.charCodeAt(i + 1) : 0;
        var c = i + 2 < str.length ? str.charCodeAt(i + 2) : 0;
        out += chars[(a >> 2) & 63];
        out += chars[((a << 4) | (b >> 4)) & 63];
        out += i + 1 < str.length ? chars[((b << 2) | (c >> 6)) & 63] : "=";
        out += i + 2 < str.length ? chars[c & 63] : "=";
    }
    return out;
}

function atob(encoded) {
    var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    var out = "";
    for (var i = 0; i < encoded.length; i += 4) {
        var a = chars.indexOf(encoded[i]);
        var b = chars.indexOf(encoded[i + 1]);
        var c = chars.indexOf(encoded[i + 2]);
        var d = chars.indexOf(encoded[i + 3]);
        out += String.fromCharCode((a << 2) | (b >> 4));
        if (c !== 64) out += String.fromCharCode(((b << 4) | (c >> 2)) & 255);
        if (d !== 64) out += String.fromCharCode(((c << 6) | d) & 255);
    }
    return out;
}

// console.time / timeEnd
var __timers__ = {};
if (typeof console !== "undefined") {
    console.time = function(label) {
        __timers__[label || "default"] = performance.now();
    };
    console.timeEnd = function(label) {
        var key = label || "default";
        if (__timers__[key] !== undefined) {
            var elapsed = performance.now() - __timers__[key];
            console.log(key + ": " + elapsed.toFixed(3) + "ms");
            delete __timers__[key];
        }
    };
}

// crypto.getRandomValues — for UUID generation
var crypto = {
    getRandomValues: function(arr) {
        for (var i = 0; i < arr.length; i++) {
            arr[i] = Math.floor(Math.random() * 256);
        }
        return arr;
    }
};

// TextEncoder / TextDecoder — UTF-8 encoding
function TextEncoder() {}
TextEncoder.prototype.encode = function(str) {
    var arr = [];
    for (var i = 0; i < str.length; i++) {
        var c = str.charCodeAt(i);
        if (c < 128) arr.push(c);
        else if (c < 2048) { arr.push(192 | (c >> 6)); arr.push(128 | (c & 63)); }
        else { arr.push(224 | (c >> 12)); arr.push(128 | ((c >> 6) & 63)); arr.push(128 | (c & 63)); }
    }
    return new Uint8Array(arr);
};

function TextDecoder() {}
TextDecoder.prototype.decode = function(buf) {
    var out = "";
    var arr = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    for (var i = 0; i < arr.length; ) {
        var b = arr[i];
        if (b < 128) { out += String.fromCharCode(b); i++; }
        else if (b < 224) { out += String.fromCharCode(((b & 31) << 6) | (arr[i+1] & 63)); i += 2; }
        else { out += String.fromCharCode(((b & 15) << 12) | ((arr[i+1] & 63) << 6) | (arr[i+2] & 63)); i += 3; }
    }
    return out;
};

// structuredClone — deep copy
function structuredClone(obj) {
    return JSON.parse(JSON.stringify(obj));
}

// fetch — minimal implementation via exec (P3)
function fetch(url, opts) {
    return new Promise(function(resolve, reject) {
        try {
            var method = (opts && opts.method) || "GET";
            var cmd = "curl -s";
            if (method !== "GET") cmd += " -X " + method;
            if (opts && opts.headers) {
                for (var k in opts.headers) cmd += " -H '" + k + ": " + opts.headers[k] + "'";
            }
            if (opts && opts.body) cmd += " -d '" + opts.body + "'";
            cmd += " '" + url + "'";
            var body = exec(cmd);
            resolve({
                ok: true, status: 200,
                text: function() { return body; },
                json: function() { return JSON.parse(body); }
            });
        } catch (e) { reject(e); }
    });
}
