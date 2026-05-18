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

        // Handle pseudo-classes separately. We pass `parsedNoPseudo`
        // (the parsed selector with pseudo stripped) to `_matchesSelector`
        // because the stylesheet wire-up path matches "would this
        // selector apply if state X were true?" — that's the structural
        // match, not the live state. The live state check is then either
        // (a) deferred to the event-handler wiring (`:hover` / `:focus` /
        // `:active`), or (b) done explicitly here (`:disabled` reads
        // `el._disabled` after the structural match). The new
        // querySelector-side _matchesSelector pseudo evaluator (pulp
        // #1737) honours pseudo when present, but is bypassed here so
        // the wire-up still sees structural matches before the user has
        // moused over anything.
        var parsedNoPseudo = parsed;
        if (parsed.pseudo) {
            parsedNoPseudo = {
                tag: parsed.tag, id: parsed.id, classes: parsed.classes,
                attrs: parsed.attrs, pseudo: null,
                parent: parsed.parent, direct: parsed.direct,
            };
        }

        if (parsed.pseudo === "hover") {
            if (_matchesSelector(el, parsedNoPseudo)) {
                _setupPseudoHover(el, rule.properties);
            }
        } else if (parsed.pseudo === "focus") {
            if (_matchesSelector(el, parsedNoPseudo)) {
                _setupPseudoFocus(el, rule.properties);
            }
        } else if (parsed.pseudo === "active") {
            if (_matchesSelector(el, parsedNoPseudo)) {
                _setupPseudoActive(el, rule.properties);
            }
        } else if (parsed.pseudo === "disabled") {
            if (_matchesSelector(el, parsedNoPseudo) && el._disabled) {
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
    },

    // pulp #2128 follow-up — document-level EventTarget with real fan-out.
    //
    // Previously these were no-ops (pulp #2101), kept that way so Three.js
    // OrbitControls' `ownerDocument.removeEventListener` didn't throw on
    // cleanup. That cleanup story is preserved (removeEventListener still
    // doesn't throw on unknown handlers) but listeners now actually fire,
    // which is needed so React "click-outside" patterns that hook
    // `document.addEventListener('mousedown', onDoc)` work — Spectr's
    // PickerDropdown, ContextMenu close-on-outside-click, and every
    // React popover that uses this pattern were silently dead before.
    //
    // The bridge dispatches into this map via `__dispatch__('document',
    // eventName, eventObj)` from C++ (see widget_bridge.cpp __dispatch__
    // preamble + claimOverlay's dismiss path).
    __eventListeners__: {},
    addEventListener: function(type, handler) {
        if (typeof type !== 'string' || typeof handler !== 'function') return;
        var list = this.__eventListeners__[type] || (this.__eventListeners__[type] = []);
        if (list.indexOf(handler) < 0) list.push(handler);
    },
    removeEventListener: function(type, handler) {
        var list = this.__eventListeners__ && this.__eventListeners__[type];
        if (!list) return;
        var idx = list.indexOf(handler);
        if (idx >= 0) list.splice(idx, 1);
    },
    dispatchEvent: function(event) {
        if (!event || typeof event !== 'object' || typeof event.type !== 'string') return true;
        var list = this.__eventListeners__ && this.__eventListeners__[event.type];
        if (!list || !list.length) return true;
        if (typeof event.preventDefault !== 'function') {
            event.preventDefault = function() { this.defaultPrevented = true; };
        }
        if (typeof event.stopPropagation !== 'function') {
            event.stopPropagation = function() {};
        }
        // Snapshot the list to tolerate handlers that
        // remove themselves during dispatch.
        var snapshot = list.slice();
        for (var i = 0; i < snapshot.length; i++) {
            try { snapshot[i](event); }
            catch (e) {
                if (typeof __dispatchError__ === 'function') {
                    __dispatchError__('document', event.type, String(e && e.stack ? e.stack : e));
                }
            }
        }
        return !event.defaultPrevented;
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


var navigator = globalThis.navigator || {};
if (typeof navigatorGPU !== "undefined" && navigatorGPU) {
    navigator.gpu = navigatorGPU;
    navigator.gpu.requestAdapter = function() {
        // pulp #2101 — prefer the native Dawn adapter when the host advertises one
        // via __describeNativeAdapterImpl. Falls back to the pure-mock adapter so
        // headless test paths (no host bridge) still resolve cleanly.
        var descriptor = null;
        if (typeof __describeNativeAdapterImpl === "function") {
            descriptor = __describeNativeAdapterImpl();
        }
        if (descriptor && descriptor.nativeBridge) {
            return Promise.resolve(__createGPUAdapter(descriptor));
        }
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
    },
    // pulp #2101 — explicit accessors for the native Dawn adapter so test
    // harnesses and demos can opt into the bridge path without relying on
    // requestAdapter's auto-detection.
    describeNativeAdapter: function() {
        if (typeof __describeNativeAdapterImpl === "function") return __describeNativeAdapterImpl();
        return null;
    },
    createNativeAdapter: function() {
        var descriptor = window.pulp.gpu.describeNativeAdapter();
        if (!descriptor || !descriptor.nativeBridge) return null;
        return __createGPUAdapter(descriptor);
    }
};
