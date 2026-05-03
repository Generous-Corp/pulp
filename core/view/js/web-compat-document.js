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
    // pulp #1323 — multiple `:hover` rules on the same element layer
    // their property maps. We keep a per-element list so `mouseleave`
    // restores the union of all hover-touched properties to their
    // pre-hover values, even when a later rule introduced a new key.
    // The list is keyed on the props *object identity* so repeated
    // _applyTo() runs (e.g. from className mutation) don't grow the
    // list — each rule object goes in exactly once per element.
    var hoverState = el._hoverState;
    if (!hoverState) {
        hoverState = el._hoverState = {
            propsList: [],
            savedProps: {},
            wired: false
        };
    }

    // Append unique rules; idempotent across repeated _applyTo() runs.
    var alreadyHave = false;
    for (var pi = 0; pi < hoverState.propsList.length; pi++) {
        if (hoverState.propsList[pi] === props) { alreadyHave = true; break; }
    }
    if (!alreadyHave) hoverState.propsList.push(props);

    // pulp #1173 — registerHover(id) arms the native dispatcher. The C++
    // side requires the widget to exist before the call lands, so we
    // defer wiring until _nativeCreated. _applyTo() runs again from
    // appendChild's _reapplyStylesheets() after _nativeCreated flips,
    // giving us a second chance to wire even for elements that matched
    // the rule pre-mount.
    if (hoverState.wired) return;
    if (!el._nativeCreated) return;
    hoverState.wired = true;

    // Use addEventListener (multi-callback __eventListeners__ map) so we
    // coexist with JSX onMouseEnter / addEventListener('mouseenter')
    // handlers the user may register independently. The lower-level
    // `on()` channel is single-callback per (id, event) — using it here
    // would clobber, or be clobbered by, any other mouseenter listener.
    // addEventListener also routes through _registerNativeEvent which
    // calls registerHover(id) for us — but only if _nativeCreated is
    // already true on this code path, which we just asserted above.
    el.addEventListener("mouseenter", function() {
        var list = hoverState.propsList;
        // Snapshot the BEFORE state for every property any rule touches.
        // Refreshed on every enter so a hover-after-className-change
        // (or a JS-driven style mutation between hovers) still reverts
        // to the pre-hover value rather than to a stale snapshot.
        for (var i = 0; i < list.length; i++) {
            var p = list[i];
            for (var k in p) {
                if (!Object.prototype.hasOwnProperty.call(hoverState.savedProps, k)) {
                    hoverState.savedProps[k] = el.style[k];
                }
            }
        }
        // Layer rules in registration order — last write wins per
        // property, which matches CSS specificity for equally-specific
        // selectors (later rules in source order win).
        for (var j = 0; j < list.length; j++) {
            _applyStyles(el, list[j]);
        }
    });
    el.addEventListener("mouseleave", function() {
        for (var k in hoverState.savedProps) {
            el.style[k] = hoverState.savedProps[k];
        }
        // Drop the snapshot so the next enter re-captures the current
        // style (which may have been mutated by JS in between).
        hoverState.savedProps = {};
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// CSS text → StyleSheet translator (pulp #1323)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Converts the contents of a `<style>` element into a StyleSheet so the
// existing rule-application path picks it up. We deliberately keep the
// parser conservative: simple selectors (tag, .class, #id) optionally
// suffixed with a single `:hover` / `:focus` / `:active` pseudo-class,
// comma-separated selector lists, and `prop: value;` declarations.
//
// Out of scope for this slice (deferred follow-ups):
//   - Descendant / child / sibling combinators in the CSS-text input
//     (the underlying matcher supports them via `_parseSelector` but
//     bringing them through the text parser opens a larger correctness
//     surface — Spectr's editor.js sticks to flat selectors).
//   - At-rules (@media, @keyframes, @supports, @import).
//   - CSS variable resolution at parse time (handled by the existing
//     style-decl path on apply).
//   - `:active` pseudo-class wiring (#1149 part b explicit non-goal).

function _stripCssComments(text) {
    return text.replace(/\/\*[\s\S]*?\*\//g, "");
}

function _parseCssDeclarations(body) {
    var props = {};
    var decls = body.split(";");
    for (var i = 0; i < decls.length; i++) {
        var d = decls[i].trim();
        if (!d) continue;
        var colon = d.indexOf(":");
        if (colon <= 0) continue;
        var name = d.slice(0, colon).trim();
        var value = d.slice(colon + 1).trim();
        if (!name || !value) continue;
        // CSS uses kebab-case; CSSStyleDeclaration._props expects camelCase.
        // The setter side handles both, but normalize here so layered
        // overrides on the same logical property collapse correctly.
        var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
        props[camel] = value;
    }
    return props;
}

function _parseCssText(text) {
    // Returns an array of { selector, properties } records — array (not
    // object) so duplicate selectors layer in source order.
    var rules = [];
    var src = _stripCssComments(text || "");
    var i = 0;
    while (i < src.length) {
        // Skip whitespace and at-rule blocks (we don't support them; just
        // jump over the matching brace pair so a stray @media doesn't
        // poison the rest of the sheet).
        var ch = src.charAt(i);
        if (ch === " " || ch === "\t" || ch === "\n" || ch === "\r") { i++; continue; }
        if (ch === "@") {
            var depth = 0, found = false;
            while (i < src.length) {
                var c2 = src.charAt(i++);
                if (c2 === "{") { depth++; found = true; }
                else if (c2 === "}") { depth--; if (depth === 0 && found) break; }
                else if (c2 === ";" && !found) { break; } // at-rule with no block
            }
            continue;
        }
        // Read selector list up to '{'
        var brace = src.indexOf("{", i);
        if (brace < 0) break;
        var selectorList = src.slice(i, brace).trim();
        var endBrace = src.indexOf("}", brace + 1);
        if (endBrace < 0) break;
        var body = src.slice(brace + 1, endBrace);
        var props = _parseCssDeclarations(body);
        // Split selector list on commas (top-level only; we don't support
        // selectors with parenthesized commas in this slice).
        var selectors = selectorList.split(",");
        for (var s = 0; s < selectors.length; s++) {
            var sel = selectors[s].trim();
            if (!sel) continue;
            rules.push({ selector: sel, properties: props });
        }
        i = endBrace + 1;
    }
    return rules;
}

function _processStyleElement(el) {
    // Detach any previously-applied sheet so re-setting textContent
    // (React commits, hot-reload) replaces rather than stacks.
    if (el._appliedSheet && typeof el._appliedSheet.detach === "function") {
        el._appliedSheet.detach();
        el._appliedSheet = null;
    }
    var rules = _parseCssText(el._textContent || "");
    if (rules.length === 0) return;
    // StyleSheet's constructor takes { selector: properties }; we feed it
    // a raw _parsedRules list to preserve duplicate selectors and
    // source-order layering for `:hover` rules.
    var sheet = Object.create(StyleSheet.prototype);
    sheet._rules = {};
    sheet._attached = false;
    sheet._parsedRules = [];
    for (var i = 0; i < rules.length; i++) {
        var r = rules[i];
        sheet._parsedRules.push({
            selector: r.selector,
            properties: r.properties,
            parsed: _parseSelector(r.selector)
        });
    }
    sheet.attach();
    el._appliedSheet = sheet;
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
    var result = { tag: null, id: null, classes: [], pseudo: null, parent: null, direct: false };

    // Split pseudo-class
    var pseudoIdx = str.indexOf(":");
    var mainPart = str;
    if (pseudoIdx >= 0) {
        result.pseudo = str.slice(pseudoIdx + 1);
        mainPart = str.slice(0, pseudoIdx);
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

function _matchesSelector(el, parsed) {
    // Match tag
    if (parsed.tag && el.tagName.toLowerCase() !== parsed.tag) return false;

    // Match id
    if (parsed.id && el.getAttribute("id") !== parsed.id) return false;

    // Match classes
    for (var i = 0; i < parsed.classes.length; i++) {
        if (!el.classList.contains(parsed.classes[i])) return false;
    }

    // Match parent constraint
    if (parsed.parent) {
        if (parsed.direct) {
            // Direct child: parent must be immediate parent
            if (!el._parentElement || !_matchesSelector(el._parentElement, parsed.parent)) return false;
        } else {
            // Descendant: any ancestor must match
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
        // Backed by an Element (`<span>`) so renderers handle text uniformly,
        // but flagged as a DOM-spec text node so reconcilers (React 18, etc.)
        // see `node.nodeType === 3` and `node.nodeName === "#text"`. Per
        // pulp #468 — React's reconciler reads both on every node it walks.
        var el = new Element("span");
        el._textContent = text;
        el._isTextNode = true;
        Object.defineProperty(el, "nodeType", { value: 3, configurable: true });
        Object.defineProperty(el, "nodeName", { value: "#text", configurable: true });
        // Spec: a Text node's `data` and `nodeValue` mirror its content.
        Object.defineProperty(el, "data", {
            get: function() { return this._textContent; },
            set: function(v) {
                this._textContent = v == null ? "" : String(v);
                if (this._nativeCreated) setText(this._id, this._textContent);
            },
            configurable: true
        });
        Object.defineProperty(el, "nodeValue", {
            get: function() { return this._textContent; },
            set: function(v) {
                this._textContent = v == null ? "" : String(v);
                if (this._nativeCreated) setText(this._id, this._textContent);
            },
            configurable: true
        });
        __elements__[el._id] = el;
        return el;
    },

    createComment: function(text) {
        // Comment nodes are invisible scaffolding for renderers (e.g. React's
        // hydration markers and ReactDOM's portal sentinels). Backed by a
        // hidden Element so DOM tree ops still work.
        var el = new Element("span");
        el._textContent = "";
        el._isCommentNode = true;
        el._hidden = true;
        Object.defineProperty(el, "nodeType", { value: 8, configurable: true });
        Object.defineProperty(el, "nodeName", { value: "#comment", configurable: true });
        Object.defineProperty(el, "data", {
            get: function() { return this._commentText || ""; },
            set: function(v) { this._commentText = v == null ? "" : String(v); },
            configurable: true
        });
        Object.defineProperty(el, "nodeValue", {
            get: function() { return this._commentText || ""; },
            set: function(v) { this._commentText = v == null ? "" : String(v); },
            configurable: true
        });
        if (text != null) el.data = text;
        __elements__[el._id] = el;
        return el;
    },

    createDocumentFragment: function() {
        // A DocumentFragment is a lightweight container that vanishes when
        // appended to a real parent (its children move, the fragment itself
        // doesn't). For our needs (React batching commits) it's sufficient
        // to model it as an Element that flattens on appendChild.
        var el = new Element("div");
        el._isDocumentFragment = true;
        Object.defineProperty(el, "nodeType", { value: 11, configurable: true });
        Object.defineProperty(el, "nodeName", { value: "#document-fragment", configurable: true });
        __elements__[el._id] = el;
        return el;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// window object (minimal shim)
// ═══════════════════════════════════════════════════════════════════════════════

var window = {
    document: document,
    getComputedStyle: getComputedStyle,
    innerWidth: 800,
    innerHeight: 600,
    devicePixelRatio: 2,
    requestAnimationFrame: function(fn) {
        // Map to Pulp's frame clock
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
    }
};

function __installGlobalIfMissing(name, value) {
    if (typeof globalThis[name] === "undefined") {
        globalThis[name] = value;
    }
    window[name] = globalThis[name];
}

__installGlobalIfMissing("GPUMapMode", {
    READ: 0x1,
    WRITE: 0x2
});

__installGlobalIfMissing("GPUShaderStage", {
    VERTEX: 0x1,
    FRAGMENT: 0x2,
    COMPUTE: 0x4
});

__installGlobalIfMissing("GPUBufferUsage", {
    MAP_READ: 0x0001,
    MAP_WRITE: 0x0002,
    COPY_SRC: 0x0004,
    COPY_DST: 0x0008,
    INDEX: 0x0010,
    VERTEX: 0x0020,
    UNIFORM: 0x0040,
    STORAGE: 0x0080,
    INDIRECT: 0x0100,
    QUERY_RESOLVE: 0x0200
});

__installGlobalIfMissing("GPUTextureUsage", {
    COPY_SRC: 0x01,
    COPY_DST: 0x02,
    TEXTURE_BINDING: 0x04,
    STORAGE_BINDING: 0x08,
    RENDER_ATTACHMENT: 0x10
});

__installGlobalIfMissing("GPUColorWrite", {
    RED: 0x1,
    GREEN: 0x2,
    BLUE: 0x4,
    ALPHA: 0x8,
    ALL: 0xF
});

function __cloneObject(source) {
    var out = {};
    if (!source) return out;
    for (var key in source) {
        if (Object.prototype.hasOwnProperty.call(source, key)) {
            out[key] = source[key];
        }
    }
    return out;
}

function __normalizedFeatureList(values, fallback) {
    var list = [];
    function pushValue(value) {
        var text = String(value);
        if (list.indexOf(text) < 0) list.push(text);
    }

    if (values && typeof values.length === "number") {
        for (var i = 0; i < values.length; ++i) pushValue(values[i]);
    }

    if (list.length === 0 && fallback && typeof fallback.length === "number") {
        for (var j = 0; j < fallback.length; ++j) pushValue(fallback[j]);
    }

    return list;
}

function __createFeatureSet(values) {
    var list = __normalizedFeatureList(values, []);
    return {
        _values: list.slice(),
        size: list.length,
        has: function(name) {
            return list.indexOf(String(name)) >= 0;
        },
        values: function() {
            return list.slice();
        },
        keys: function() {
            return list.slice();
        },
        forEach: function(fn, thisArg) {
            for (var i = 0; i < list.length; ++i) {
                fn.call(thisArg, list[i], list[i], this);
            }
        }
    };
}

function __defaultMockGpuLimits() {
    return {
        maxTextureDimension2D: 4096,
        maxColorAttachments: 4,
        maxBindGroups: 4,
        maxBufferSize: 16777216,
        maxStorageBufferBindingSize: 16777216,
        maxUniformBufferBindingSize: 65536
    };
}

function __mergeMockGpuLimits(overrides) {
    var limits = __defaultMockGpuLimits();
    overrides = overrides || {};
    for (var key in overrides) {
        if (Object.prototype.hasOwnProperty.call(overrides, key)) {
            limits[key] = overrides[key];
        }
    }
    return limits;
}

function __mockGpuInfo() {
    if (typeof getGPUInfo === "function") return getGPUInfo();
    return { available: false, backend: "unavailable" };
}

function __mockPreferredCanvasFormat() {
    if (typeof navigatorGPU !== "undefined" && navigatorGPU
            && typeof navigatorGPU.getPreferredCanvasFormat === "function") {
        return navigatorGPU.getPreferredCanvasFormat();
    }
    return "bgra8unorm";
}

function __textureExtent(sizeLike) {
    if (Array.isArray(sizeLike)) {
        return {
            width: sizeLike[0] || 1,
            height: sizeLike[1] || 1,
            depthOrArrayLayers: sizeLike[2] || 1
        };
    }
    sizeLike = sizeLike || {};
    return {
        width: sizeLike.width || sizeLike.inlineSize || 1,
        height: sizeLike.height || sizeLike.blockSize || 1,
        depthOrArrayLayers: sizeLike.depthOrArrayLayers || sizeLike.depth || 1
    };
}

function __createMockGPUBuffer(init) {
    init = init || {};
    var buffer = {
        _objectName: "GPUBuffer",
        label: init.label || "",
        size: init.size || 0,
        usage: init.usage || 0,
        mapState: "unmapped",
        _destroyed: false,
        _bytes: new Uint8Array(init.size || 0)
    };
    buffer.mapAsync = function() {
        buffer.mapState = "mapped";
        return Promise.resolve(undefined);
    };
    buffer.getMappedRange = function(offset, size) {
        var begin = offset || 0;
        var end = size == null ? buffer.size : begin + size;
        return buffer._bytes.buffer.slice(begin, end);
    };
    buffer.unmap = function() { buffer.mapState = "unmapped"; };
    buffer.destroy = function() { buffer._destroyed = true; };
    return buffer;
}

function __createMockGPUTextureView(init) {
    init = init || {};
    return {
        _objectName: "GPUTextureView",
        label: init.label || "",
        format: init.format || __mockPreferredCanvasFormat(),
        dimension: init.dimension || "2d",
        aspect: init.aspect || "all",
        texture: init.texture || null
    };
}

function __createMockGPUTexture(init) {
    init = init || {};
    var size = __textureExtent(init.size);
    var texture = {
        _objectName: "GPUTexture",
        label: init.label || "",
        width: size.width,
        height: size.height,
        depthOrArrayLayers: size.depthOrArrayLayers,
        dimension: init.dimension || "2d",
        format: init.format || __mockPreferredCanvasFormat(),
        usage: init.usage || GPUTextureUsage.RENDER_ATTACHMENT,
        mipLevelCount: init.mipLevelCount || 1,
        sampleCount: init.sampleCount || 1,
        _destroyed: false
    };
    texture.createView = function(descriptor) {
        descriptor = descriptor || {};
        return __createMockGPUTextureView({
            label: descriptor.label || texture.label,
            format: descriptor.format || texture.format,
            dimension: descriptor.dimension || texture.dimension,
            aspect: descriptor.aspect || "all",
            texture: texture
        });
    };
    texture.destroy = function() { texture._destroyed = true; };
    return texture;
}

function __createMockGPUCommandBuffer(init) {
    init = init || {};
    return {
        _objectName: "GPUCommandBuffer",
        label: init.label || ""
    };
}

function __createMockGPURenderPassEncoder(init) {
    init = init || {};
    return {
        _objectName: "GPURenderPassEncoder",
        label: init.label || "",
        setPipeline: function() {},
        setBindGroup: function() {},
        setVertexBuffer: function() {},
        setIndexBuffer: function() {},
        setViewport: function() {},
        setScissorRect: function() {},
        setStencilReference: function() {},
        draw: function() {},
        drawIndexed: function() {},
        end: function() {}
    };
}

function __createMockGPUComputePassEncoder(init) {
    init = init || {};
    var currentComputePipeline = null;
    var computeBindGroups = {};
    var computeCommands = [];

    return {
        _objectName: "GPUComputePassEncoder",
        label: init.label || "",
        _commands: computeCommands,
        setPipeline: function(pipeline) {
            currentComputePipeline = pipeline;
        },
        setBindGroup: function(index, bindGroup) {
            computeBindGroups[index == null ? 0 : index] = bindGroup || null;
        },
        dispatchWorkgroups: function(x, y, z) {
            computeCommands.push({
                type: "dispatch",
                pipeline: currentComputePipeline,
                bindGroups: Object.assign({}, computeBindGroups),
                workgroupCountX: x || 1,
                workgroupCountY: y || 1,
                workgroupCountZ: z || 1
            });
        },
        dispatchWorkgroupsIndirect: function(indirectBuffer, indirectOffset) {
            computeCommands.push({
                type: "dispatch-indirect",
                pipeline: currentComputePipeline,
                bindGroups: Object.assign({}, computeBindGroups),
                indirectBuffer: indirectBuffer,
                indirectOffset: indirectOffset || 0
            });
        },
        end: function() {
            // Commands are captured in _commands for native dispatch
        }
    };
}

function __createMockGPUCommandEncoder(init) {
    init = init || {};
    var computePasses = [];
    return {
        _objectName: "GPUCommandEncoder",
        label: init.label || "",
        _computePasses: computePasses,
        beginRenderPass: function(descriptor) {
            return __createMockGPURenderPassEncoder({
                label: descriptor && descriptor.label ? descriptor.label : "",
                descriptor: descriptor || {}
            });
        },
        beginComputePass: function(descriptor) {
            var pass = __createMockGPUComputePassEncoder({ label: descriptor && descriptor.label ? descriptor.label : "" });
            computePasses.push(pass);
            return pass;
        },
        copyBufferToBuffer: function() {},
        copyTextureToBuffer: function() {},
        copyBufferToTexture: function() {},
        finish: function(descriptor) {
            var cmdBuf = __createMockGPUCommandBuffer({ label: descriptor && descriptor.label ? descriptor.label : "" });
            // Attach compute pass commands to the command buffer for native dispatch
            cmdBuf._computePasses = computePasses;
            return cmdBuf;
        }
    };
}

function __createMockGPUShaderModule(init) {
    init = init || {};
    return {
        _objectName: "GPUShaderModule",
        label: init.label || "",
        code: init.code || "",
        getCompilationInfo: function() {
            return Promise.resolve({ messages: [] });
        }
    };
}

function __createMockGPUBindGroupLayout(init) {
    init = init || {};
    return {
        _objectName: "GPUBindGroupLayout",
        label: init.label || "",
        entries: init.entries || []
    };
}

function __createMockGPUBindGroup(init) {
    init = init || {};
    return {
        _objectName: "GPUBindGroup",
        label: init.label || "",
        layout: init.layout || null,
        entries: init.entries || []
    };
}

function __createMockGPUPipelineLayout(init) {
    init = init || {};
    return {
        _objectName: "GPUPipelineLayout",
        label: init.label || "",
        bindGroupLayouts: init.bindGroupLayouts || []
    };
}

function __createMockGPURenderPipeline(init) {
    init = init || {};
    var pipeline = {
        _objectName: "GPURenderPipeline",
        label: init.label || "",
        _bindGroupLayouts: init.bindGroupLayouts || []
    };
    pipeline.getBindGroupLayout = function(index) {
        return pipeline._bindGroupLayouts[index] || __createMockGPUBindGroupLayout({});
    };
    return pipeline;
}

function __createMockGPUSampler(init) {
    init = init || {};
    return {
        _objectName: "GPUSampler",
        label: init.label || "",
        addressModeU: init.addressModeU || "clamp-to-edge",
        addressModeV: init.addressModeV || "clamp-to-edge",
        magFilter: init.magFilter || "nearest",
        minFilter: init.minFilter || "nearest"
    };
}

function __createMockGPUQueue(init) {
    init = init || {};
    var queue = {
        _objectName: "GPUQueue",
        label: init.label || "",
        _submitCount: 0
    };
    queue.submit = function(commandBuffers) {
        queue._submitCount += commandBuffers && typeof commandBuffers.length === "number" ? commandBuffers.length : 0;
    };
    queue.writeBuffer = function(buffer, bufferOffset, data, dataOffset, size) {
        if (!buffer || buffer._objectName !== "GPUBuffer") return;
        var source = __toUint8Array(data);
        var begin = bufferOffset || 0;
        var sliceOffset = dataOffset || 0;
        var sliceSize = size == null ? source.length - sliceOffset : size;
        buffer._bytes.set(source.slice(sliceOffset, sliceOffset + sliceSize), begin);
    };
    queue.writeTexture = function(destination, data, dataLayout, size) {
        if (!destination || !destination.texture) return;
        var texture = destination.texture;
        var source = __toUint8Array(data);
        texture._bytes = source;
        texture._bytesPerRow = dataLayout && dataLayout.bytesPerRow ? dataLayout.bytesPerRow : 0;
        texture._rowsPerImage = dataLayout && dataLayout.rowsPerImage ? dataLayout.rowsPerImage : (size && size[1] ? size[1] : texture.height || 1);
    };
    queue.copyExternalImageToTexture = function(source, destination, copySize) {
        if (!source || !destination || !destination.texture) return;
        var imageBitmap = source.source;
        if (!imageBitmap || !imageBitmap._decodedPixels) return;
        var texture = destination.texture;
        texture._bytes = imageBitmap._decodedPixels;
        texture._bytesPerRow = imageBitmap.width * 4;
        texture._rowsPerImage = imageBitmap.height;
        texture.width = imageBitmap.width;
        texture.height = imageBitmap.height;
    };
    queue.onSubmittedWorkDone = function() {
        return Promise.resolve(undefined);
    };
    return queue;
}

function __pickDeviceFeatures(adapter, descriptor) {
    var requested = descriptor && descriptor.requiredFeatures ? descriptor.requiredFeatures : [];
    var available = adapter && adapter.features ? adapter.features.values() : [];
    if (!requested || requested.length === 0) return available;
    var picked = [];
    for (var i = 0; i < requested.length; ++i) {
        var feature = String(requested[i]);
        if (available.indexOf(feature) >= 0 && picked.indexOf(feature) < 0) {
            picked.push(feature);
        }
    }
    if (picked.indexOf("core-features-and-limits") < 0) {
        picked.push("core-features-and-limits");
    }
    return picked;
}

function __createMockGPUDevice(adapter, descriptor) {
    descriptor = descriptor || {};
    var device = {
        _objectName: "GPUDevice",
        label: descriptor.label || "",
        features: __createFeatureSet(__pickDeviceFeatures(adapter, descriptor)),
        limits: __mergeMockGpuLimits(descriptor.requiredLimits),
        queue: __createMockGPUQueue({}),
        adapterInfo: adapter && adapter.info ? adapter.info : null,
        lost: new Promise(function() {}),
        _destroyed: false
    };
    device.createBuffer = function(bufferDescriptor) { return __createMockGPUBuffer(bufferDescriptor || {}); };
    device.createTexture = function(textureDescriptor) { return __createMockGPUTexture(textureDescriptor || {}); };
    device.createSampler = function(samplerDescriptor) { return __createMockGPUSampler(samplerDescriptor || {}); };
    device.createShaderModule = function(shaderDescriptor) { return __createMockGPUShaderModule(shaderDescriptor || {}); };
    device.createBindGroupLayout = function(layoutDescriptor) { return __createMockGPUBindGroupLayout(layoutDescriptor || {}); };
    device.createBindGroup = function(bindGroupDescriptor) { return __createMockGPUBindGroup(bindGroupDescriptor || {}); };
    device.createPipelineLayout = function(layoutDescriptor) { return __createMockGPUPipelineLayout(layoutDescriptor || {}); };
    device.createRenderPipeline = function(pipelineDescriptor) {
        pipelineDescriptor = pipelineDescriptor || {};
        return __createMockGPURenderPipeline({
            label: pipelineDescriptor.label || "",
            bindGroupLayouts: pipelineDescriptor.layout && pipelineDescriptor.layout.bindGroupLayouts
                ? pipelineDescriptor.layout.bindGroupLayouts : []
        });
    };
    device.createComputePipeline = function(descriptor) {
        descriptor = descriptor || {};
        var compute = descriptor.compute || {};
        var pipeline = {
            _objectName: "GPUComputePipeline",
            label: descriptor.label || "",
            _compute: compute,
            _nativeBridge: device._nativeBridge || false,
            _bindGroupLayouts: descriptor.layout && descriptor.layout.bindGroupLayouts
                ? descriptor.layout.bindGroupLayouts : []
        };
        pipeline.getBindGroupLayout = function(index) {
            return pipeline._bindGroupLayouts[index] || __createMockGPUBindGroupLayout({});
        };
        return pipeline;
    };
    device.createComputePipelineAsync = function(descriptor) {
        return Promise.resolve(device.createComputePipeline(descriptor));
    };
    device.createRenderPipelineAsync = function(descriptor) {
        return Promise.resolve(device.createRenderPipeline(descriptor));
    };
    device.createCommandEncoder = function(commandDescriptor) { return __createMockGPUCommandEncoder(commandDescriptor || {}); };
    device.destroy = function() { device._destroyed = true; };
    return device;
}

function __createMockGPUAdapter(init) {
    init = init || {};
    var adapter = {
        _objectName: "GPUAdapter",
        name: init.name || "Mock Dawn Adapter",
        backend: init.backend || __mockGpuInfo().backend,
        preferredCanvasFormat: init.preferredCanvasFormat || __mockPreferredCanvasFormat(),
        features: __createFeatureSet(init.features || [ "core-features-and-limits", "timestamp-query" ]),
        limits: __mergeMockGpuLimits(init.limits),
        info: init.info || { vendor: "Pulp", architecture: init.backend || __mockGpuInfo().backend, description: init.name || "Mock Dawn Adapter" }
    };
    adapter.requestDevice = function(descriptor) {
        return Promise.resolve(__createMockGPUDevice(adapter, descriptor || {}));
    };
    return adapter;
}

function __createMockGPUCanvasContext(canvasEl) {
    var context = {
        _objectName: "GPUCanvasContext",
        canvas: canvasEl,
        _configured: false,
        device: null,
        format: __mockPreferredCanvasFormat(),
        usage: GPUTextureUsage.RENDER_ATTACHMENT,
        alphaMode: "opaque"
    };
    context.configure = function(descriptor) {
        descriptor = descriptor || {};
        context._configured = true;
        context.device = descriptor.device || null;
        context.format = descriptor.format || __mockPreferredCanvasFormat();
        context.usage = descriptor.usage || GPUTextureUsage.RENDER_ATTACHMENT;
        context.alphaMode = descriptor.alphaMode || "opaque";
    };
    context.getCurrentTexture = function() {
        return __createMockGPUTexture({
            size: {
                width: context.canvas && context.canvas.width ? context.canvas.width : 1,
                height: context.canvas && context.canvas.height ? context.canvas.height : 1
            },
            format: context.format,
            usage: context.usage,
            label: (context.canvas && context.canvas.id ? context.canvas.id : "pulp-canvas") + "-current-texture"
        });
    };
    context.present = function() {};
    return context;
}

var navigator = globalThis.navigator || {};
if (typeof navigatorGPU !== "undefined" && navigatorGPU) {
    navigator.gpu = navigatorGPU;
    navigator.gpu.requestAdapter = function() {
        return Promise.resolve(window.pulp.gpu.createMockAdapter());
    };
}
window.navigator = navigator;
globalThis.navigator = navigator;

var performance = {
    now: function() {
        if (typeof __performanceNow__ === "function") return __performanceNow__();
        return Date.now();
    }
};
window.performance = performance;
globalThis.performance = performance;

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
    clear: function() {},
    get length() { return 0; },
    key: function() { return null; }
};
window.localStorage = localStorage;
globalThis.localStorage = localStorage;
window.sessionStorage = localStorage;
globalThis.sessionStorage = localStorage;

function Image() {
    var self = this;
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
window.Image = Image;
globalThis.Image = Image;

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
window.btoa = btoa;
globalThis.btoa = btoa;

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
window.atob = atob;
globalThis.atob = atob;

var crypto = {
    getRandomValues: function(arr) {
        for (var i = 0; i < arr.length; i++) {
            arr[i] = Math.floor(Math.random() * 256);
        }
        return arr;
    }
};
window.crypto = crypto;
globalThis.crypto = crypto;

function TextEncoder() {}
TextEncoder.prototype.encode = function(str) {
    var arr = [];
    for (var i = 0; i < str.length; i++) {
        var c = str.charCodeAt(i);
        if (c < 128) arr.push(c);
        else if (c < 2048) {
            arr.push(192 | (c >> 6));
            arr.push(128 | (c & 63));
        } else {
            arr.push(224 | (c >> 12));
            arr.push(128 | ((c >> 6) & 63));
            arr.push(128 | (c & 63));
        }
    }
    return new Uint8Array(arr);
};
window.TextEncoder = TextEncoder;
globalThis.TextEncoder = TextEncoder;

function TextDecoder() {}
TextDecoder.prototype.decode = function(buf) {
    var out = "";
    var arr = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    for (var i = 0; i < arr.length; ) {
        var b = arr[i];
        if (b < 128) {
            out += String.fromCharCode(b);
            i++;
        } else if (b < 224) {
            out += String.fromCharCode(((b & 31) << 6) | (arr[i + 1] & 63));
            i += 2;
        } else {
            out += String.fromCharCode(((b & 15) << 12) | ((arr[i + 1] & 63) << 6) | (arr[i + 2] & 63));
            i += 3;
        }
    }
    return out;
};
window.TextDecoder = TextDecoder;
globalThis.TextDecoder = TextDecoder;

function __bytesFromBase64(encoded) {
    var binary = atob(encoded || "");
    var bytes = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; ++i) {
        bytes[i] = binary.charCodeAt(i) & 255;
    }
    return bytes;
}

function __bytesToBase64(bytes) {
    var binary = "";
    for (var i = 0; i < bytes.length; ++i) {
        binary += String.fromCharCode(bytes[i] & 255);
    }
    return btoa(binary);
}

function __canonicalDataUriMimeType(mimeType) {
    if (!mimeType) return "application/octet-stream";
    var lower = String(mimeType).toLowerCase();
    if (lower === "application/json" || lower === "text/json") {
        return "application/json;charset=utf-8";
    }
    return String(mimeType);
}

function __toUint8Array(value) {
    if (value == null) return new Uint8Array(0);
    if (value instanceof Uint8Array) return value;
    if (value instanceof ArrayBuffer) return new Uint8Array(value);
    if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    if (Array.isArray(value)) return new Uint8Array(value);
    if (typeof value === "string") return new TextEncoder().encode(value);
    return new TextEncoder().encode(String(value));
}

function __toArrayBuffer(bytes) {
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

function PulpHeaders(init) {
    this._map = {};
    if (!init) return;
    for (var key in init) {
        if (Object.prototype.hasOwnProperty.call(init, key)) {
            this.set(key, init[key]);
        }
    }
}
PulpHeaders.prototype.get = function(name) {
    var key = String(name || "").toLowerCase();
    return Object.prototype.hasOwnProperty.call(this._map, key) ? this._map[key] : null;
};
PulpHeaders.prototype.set = function(name, value) {
    this._map[String(name || "").toLowerCase()] = String(value == null ? "" : value);
};
var __PulpHeaders = typeof globalThis.Headers !== "undefined" ? globalThis.Headers : PulpHeaders;
if (typeof globalThis.Headers === "undefined") {
    globalThis.Headers = __PulpHeaders;
}

function PulpBlob(parts, options) {
    var chunks = [];
    var size = 0;
    var sourceParts = parts || [];
    for (var i = 0; i < sourceParts.length; ++i) {
        var bytes = __toUint8Array(sourceParts[i]);
        chunks.push(bytes);
        size += bytes.length;
    }

    var merged = new Uint8Array(size);
    var offset = 0;
    for (var j = 0; j < chunks.length; ++j) {
        merged.set(chunks[j], offset);
        offset += chunks[j].length;
    }

    this._bytes = merged;
    this.size = merged.length;
    this.type = options && options.type ? String(options.type) : "";
}
PulpBlob.prototype.arrayBuffer = function() {
    return __toArrayBuffer(this._bytes);
};
PulpBlob.prototype.text = function() {
    return new TextDecoder().decode(this._bytes);
};
var __PulpBlob = typeof globalThis.Blob !== "undefined" ? globalThis.Blob : PulpBlob;
if (typeof globalThis.Blob === "undefined") {
    globalThis.Blob = __PulpBlob;
}

function PulpResponse(body, init) {
    init = init || {};
    this.status = init.status == null ? 200 : init.status;
    this.ok = this.status >= 200 && this.status < 300;
    this.statusText = init.statusText || "";
    this.url = init.url || "";
    this.headers = init.headers instanceof __PulpHeaders ? init.headers : new __PulpHeaders(init.headers || {});
    if (init.contentType && !this.headers.get("content-type")) {
        this.headers.set("content-type", init.contentType);
    }
    this._bytes = __toUint8Array(body);
}
PulpResponse.prototype.arrayBuffer = function() {
    return __toArrayBuffer(this._bytes);
};
PulpResponse.prototype.text = function() {
    return new TextDecoder().decode(this._bytes);
};
PulpResponse.prototype.json = function() {
    return JSON.parse(this.text());
};
PulpResponse.prototype.blob = function() {
    return new __PulpBlob([this._bytes], { type: this.headers.get("content-type") || "" });
};
PulpResponse.prototype.clone = function() {
    return new __PulpResponse(this._bytes.slice(0), {
        status: this.status,
        statusText: this.statusText,
        url: this.url,
        headers: this.headers
    });
};
var __PulpResponse = typeof globalThis.Response !== "undefined" ? globalThis.Response : PulpResponse;
if (typeof globalThis.Response === "undefined") {
    globalThis.Response = __PulpResponse;
}

function createImageBitmap(source) {
    var bytes;
    if (source && source._bytes) {
        bytes = source._bytes;
    } else if (source instanceof ArrayBuffer) {
        bytes = new Uint8Array(source);
    } else if (source instanceof Uint8Array) {
        bytes = source;
    } else {
        return Promise.reject(new Error("createImageBitmap: unsupported source type"));
    }

    if (typeof __decodeImageDataImpl === "function") {
        var payload = JSON.stringify({ data: Array.from(bytes) });
        var result = __decodeImageDataImpl(payload);
        if (result && result.ok) {
            var bitmap = {
                width: result.width,
                height: result.height,
                _decodedPixels: new Uint8Array(result.pixels),
                close: function() {}
            };
            return Promise.resolve(bitmap);
        }
        return Promise.reject(new Error("createImageBitmap: failed to decode image"));
    }

    return Promise.reject(new Error("createImageBitmap: no native decoder available"));
}
if (typeof globalThis.createImageBitmap === "undefined") {
    globalThis.createImageBitmap = createImageBitmap;
}

function PulpURL(url) {
    this.href = String(url || "");
}
PulpURL.createObjectURL = function(blobLike) {
    var blob = blobLike instanceof PulpBlob ? blobLike : new PulpBlob([blobLike]);
    return "data:" + __canonicalDataUriMimeType(blob.type || "application/octet-stream")
        + ";base64," + __bytesToBase64(blob._bytes);
};
PulpURL.revokeObjectURL = function() {};
var __PulpURL = typeof globalThis.URL !== "undefined" ? globalThis.URL : PulpURL;
if (typeof __PulpURL.createObjectURL !== "function") {
    __PulpURL.createObjectURL = PulpURL.createObjectURL;
}
if (typeof __PulpURL.revokeObjectURL !== "function") {
    __PulpURL.revokeObjectURL = PulpURL.revokeObjectURL;
}
if (typeof globalThis.URL === "undefined") {
    globalThis.URL = __PulpURL;
}

function __responseFromDataUri(uri, sourceUrl) {
    var text = String(uri || "");
    var comma = text.indexOf(",");
    if (comma < 0) throw new Error("Malformed data URI");
    var meta = text.slice(5, comma);
    var payload = text.slice(comma + 1);
    var isBase64 = /;base64$/i.test(meta);
    var mime = meta.replace(/;base64$/i, "") || "application/octet-stream";
    var bytes = isBase64 ? __bytesFromBase64(payload) : new TextEncoder().encode(decodeURIComponent(payload));
    return new __PulpResponse(bytes, {
        status: 200,
        url: sourceUrl || text,
        contentType: __canonicalDataUriMimeType(mime)
    });
}

function __responseFromAssetRecord(record) {
    return new __PulpResponse(__bytesFromBase64(record && record.base64 ? record.base64 : ""), {
        status: record && record.status != null ? record.status : 404,
        url: record && record.url ? record.url : "",
        contentType: record && record.contentType ? record.contentType : "application/octet-stream"
    });
}

function __pulpFetch(url) {
    var requestUrl = String(url || "");
    return new Promise(function(resolve, reject) {
        try {
            if (requestUrl.indexOf("data:") === 0) {
                resolve(__responseFromDataUri(requestUrl, requestUrl));
                return;
            }

            if (typeof __loadAssetSync__ !== "function") {
                reject(new Error("Asset bridge unavailable"));
                return;
            }

            var record = __loadAssetSync__(requestUrl);
            if (!record || !record.ok) {
                var error = new Error("Failed to fetch asset: " + requestUrl);
                error.status = record && record.status ? record.status : 404;
                reject(error);
                return;
            }

            resolve(__responseFromAssetRecord(record));
        } catch (error) {
            reject(error);
        }
    });
}
if (typeof globalThis.fetch === "undefined") {
    globalThis.fetch = __pulpFetch;
}
window.pulp = window.pulp || {};
window.pulp.fetch = __pulpFetch;

function structuredClone(obj) {
    return JSON.parse(JSON.stringify(obj));
}
window.structuredClone = structuredClone;
globalThis.structuredClone = structuredClone;

window.pulp = window.pulp || {};
window.pulp.gpu = {
    getInfo: function() {
        if (typeof getGPUInfo === "function") return getGPUInfo();
        return { available: false, backend: "unavailable" };
    },
    createMockAdapter: function() {
        var info = __mockGpuInfo();
        return __createMockGPUAdapter({
            backend: info.backend,
            preferredCanvasFormat: __mockPreferredCanvasFormat()
        });
    },
    createMockDevice: function(adapter, descriptor) {
        adapter = adapter && adapter._objectName === "GPUAdapter" ? adapter : window.pulp.gpu.createMockAdapter();
        descriptor = descriptor || {};
        return __createMockGPUDevice(adapter, descriptor);
    }
};
