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
        if (tag === "label") {
            // pulp DIVERGE→PASS sweep — wire `<label for="x">` click
            // routing to focus / toggle the labeled input. Note: this
            // branch only runs in the createElement+later-mount path;
            // the appendChild fast path goes through __domAppend on the
            // C++ side and never re-enters JS, so we ALSO install the
            // routing in `setAttribute("for", ...)` below. Idempotent.
            this._installLabelForRouting();
        }
    } else if (tag === "svg") {
        // pulp #1147 — inline SVGs in web-compat code (Spectr's mode-icon
        // popover rows, React-rendered icons) are leaf containers. We
        // don't ship an SVG renderer, but we MUST honor the HTML
        // `width`/`height` attributes so the flex parent reserves layout
        // space. Without this the row collapses to height:0 and the
        // sibling text paints over a blank gutter. Width/height attribute
        // replay happens in the shared block below.
        createCol(id, "");
    } else if (tag === "rect") {
        // pulp #1926 — SVG <rect> primitive. Spectr emits these for
        // toggle-pill backgrounds and segmented-control fills.
        // Geometry (x/y/width/height) + fill/stroke replayed below
        // through __replaySvgRectAttributes__.
        if (typeof createSvgRect === "function") {
            createSvgRect(id, "");
        } else {
            createCol(id, "");
        }
    } else if (tag === "line") {
        // pulp #1926 — SVG <line> primitive. Spectr uses these for
        // analyzer-line indicators next to PEAK/AVG/BOTH/OFF and as
        // ruler ticks. Endpoints (x1/y1/x2/y2) + stroke replayed below.
        if (typeof createSvgLine === "function") {
            createSvgLine(id, "");
        } else {
            createCol(id, "");
        }
    } else if (tag === "circle") {
        // pulp #1926 — SVG <circle> → SvgPath with synthesized `d`
        // path. The path is computed in __replaySvgCircleAttributes__
        // from cx/cy/r attributes after mount.
        if (typeof createSvgPath === "function") {
            createSvgPath(id, "");
        } else {
            createCol(id, "");
        }
    } else if (tag === "path") {
        // pulp #1899 — Spectr's React-rendered icon glyphs emit raw
        // `<svg><path d="..." stroke="currentColor" .../></svg>` JSX. The
        // SvgPath native widget + bridge surface (createSvgPath /
        // setSvgPath / setSvgStroke / setSvgFill / setSvgStrokeWidth)
        // already exist (pulp #994 / #1416), but the web-compat shim
        // routed `<path>` into the unknown-tag default (createCol),
        // producing an empty box. Wire <path> directly to createSvgPath
        // and replay `d` / `stroke` / `stroke-width` / `fill` /
        // `viewBox` (inherited from the parent <svg>) through
        // __replaySvgPathAttributes__ at the end of this function.
        if (typeof createSvgPath === "function") {
            createSvgPath(id, "");
        } else {
            createCol(id, "");
        }
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
            // pulp #1899 — `<input type="range">` defaults to HORIZONTAL.
            // HTML semantics (and Spectr's MorphSlider, Web Audio demos,
            // CSS-Tricks / MDN examples) treat the range slider as
            // horizontal unless an explicit hint says otherwise. Pulp
            // previously hard-coded "vertical" which collapsed every
            // imported web slider to a tall fader, painting nothing in
            // the typical 90px-wide flex row.
            //
            // Heuristic (in priority order):
            //   1. `aria-orientation="vertical"` → vertical
            //   2. inline `style.height > style.width` → vertical
            //   3. otherwise → horizontal (HTML default)
            createFader(id, __resolveRangeOrientation__(this), "");
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
        // pulp #1658 — wire <img> to ImageView via createImage (was: createLabel
        // placeholder). ImageView::paint shows an "IMG" placeholder when path
        // is empty, then decodes via Skia draw_image_from_file once
        // setImageSource is called from setAttribute('src', …).
        createImage(id, "");
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
    // pulp Wave 3 html.2 / #1476 — replay any ARIA attributes that were
    // set on this element before the native widget existed.
    if (typeof __replayAriaAttributes__ === "function") {
        __replayAriaAttributes__(this);
    }
    // pulp #1926 — replay rect / line / circle SVG attributes captured
    // pre-mount. React/JSX commits attributes before appendChild
    // materializes the node, so by the time setAttribute('x', ...) /
    // ('x1', ...) / ('cx', ...) lands the bridge has no native id yet.
    // The replay functions flush geometry + fill / stroke / stroke-width
    // from _attributes once `_nativeCreated` flips true.
    if (typeof __replaySvgRectAttributes__ === "function") {
        __replaySvgRectAttributes__(this);
    }
    if (typeof __replaySvgLineAttributes__ === "function") {
        __replaySvgLineAttributes__(this);
    }
    if (typeof __replaySvgCircleAttributes__ === "function") {
        __replaySvgCircleAttributes__(this);
    }
    // pulp #1899 — replay <path> SVG attributes captured pre-mount
    // (React/JSX commits attributes before appendChild materializes the
    // node, so by the time setAttribute('d', ...) lands the bridge has
    // no native id yet). __replaySvgPathAttributes__ flushes d / stroke
    // / stroke-width / fill from _attributes and inherits viewBox from
    // the parent <svg>.
    if (typeof __replaySvgPathAttributes__ === "function") {
        __replaySvgPathAttributes__(this);
    }
};

// pulp #1899 — orientation heuristic for <input type="range">. Returns
// "horizontal" (HTML default) or "vertical". Priority:
//   1. aria-orientation attribute explicitly says "vertical" (or "horizontal")
//   2. inline style.height > inline style.width — BOTH must be inline-set;
//      otherwise width may come from a flex parent (not visible here) and
//      the comparison is meaningless. The earlier "hasH && !hasW → vertical"
//      shortcut regressed horizontal sliders whose width came from layout
//      (Codex P1 review of #1917).
//   3. otherwise horizontal (the HTML / Web-Audio convention).
// TODO: add test for #1917 P1 regression (horizontal slider with
//       inline style.height but width supplied by flex parent).
function __resolveRangeOrientation__(el) {
    if (!el) return "horizontal";
    var aria = el._attributes && el._attributes["aria-orientation"];
    if (aria === "vertical") return "vertical";
    if (aria === "horizontal") return "horizontal";
    // Read pre-mount inline-style props captured on the CSSStyleDeclaration.
    // Both width AND height must be inline-explicit before we trust the
    // comparison — see header comment for the flex-width regression.
    var s = el.style && el.style._props;
    if (s) {
        var w = parseFloat(s.width);
        var h = parseFloat(s.height);
        var hasW = w === w && w > 0;          // NaN-safe
        var hasH = h === h && h > 0;
        if (hasH && hasW && h > w) return "vertical";
    }
    return "horizontal";
}

// pulp #1917 (Codex P1) — SVG `currentColor` resolution.
//
// The SVG spec resolves `stroke="currentColor"` / `fill="currentColor"`
// to the element's CSS `color` property at render time. The C++
// SvgPathWidget bridge does not parse the literal token, so without
// resolution the stroke renders transparent/black/garbage depending on
// the backend. Walk up the parent chain looking for an inline
// `style.color` (color is inherited per CSS); fall back to "black" per
// SVG spec when nothing is set anywhere on the chain.
function __resolveCurrentColor__(el) {
    var anc = el;
    while (anc) {
        var col = anc.style && anc.style._props && anc.style._props.color;
        if (typeof col === "string" && col.length > 0 && col !== "inherit") {
            return col;
        }
        anc = anc._parentElement;
    }
    return "black"; // SVG spec default
}

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
    if (typeof setFlex === "function") {
        var w = el._attributes.width;
        var h = el._attributes.height;
        if (w !== undefined) {
            var pw = parseFloat(w); if (pw === pw) setFlex(el._id, "width", pw);
        }
        if (h !== undefined) {
            var ph = parseFloat(h); if (ph === ph) setFlex(el._id, "height", ph);
        }
    }
    // pulp #1658 — replay src for <img> elements through to setImageSource
    // so the React/JSX setAttribute-before-mount path works (attributes
    // captured pre-mount in _attributes, flushed once the native widget
    // is created via _ensureNative or a later appendChild).
    if (tag === "img" && typeof setImageSource === "function") {
        var src = el._attributes.src;
        if (src !== undefined) setImageSource(el._id, String(src));
    }
}

// pulp Wave 3 html.2 / #1476 — replay ARIA attributes (`aria-label` /
// `role`) when the native widget comes online.  React/JSX commits
// attributes before `appendChild` mounts the node, so by the time
// `setAttribute('aria-label', ...)` runs the bridge has no native id
// yet; we capture the value in `_attributes` (the existing path) and
// flush it through the bridge once `_nativeCreated` is true.
// Idempotent: safe to call from `_ensureNative` and from any later
// mount path (`appendChild`, `insertBefore`, etc.).
function __replayAriaAttributes__(el) {
    if (!el || !el._nativeCreated || !el._attributes) return;
    var label = el._attributes["aria-label"];
    if (label !== undefined && typeof setAccessibilityLabel === "function") {
        setAccessibilityLabel(el._id, String(label));
    }
    var role = el._attributes["role"];
    if (role !== undefined && typeof setAccessibilityRole === "function") {
        setAccessibilityRole(el._id, String(role));
    }
    // pulp #1737 — replay ARIA state attributes via setAccessibilityState.
    // React commits attributes before mount, so we have to defer the
    // bridge call until _nativeCreated. Each state attribute routes
    // through one bridge fn; the C++ side dispatches by attr name.
    if (typeof setAccessibilityState === "function") {
        var states = ["pressed", "checked", "disabled", "hidden"];
        for (var si = 0; si < states.length; si++) {
            var key = "aria-" + states[si];
            var val = el._attributes[key];
            if (val !== undefined) {
                setAccessibilityState(el._id, states[si], String(val));
            }
        }
    }
}

// pulp #1926 — replay <rect> SVG attributes through the SvgRectWidget
// bridge. Mirrors the pre-mount-replay pattern used for ARIA + media:
// React/JSX commits setAttribute() before appendChild materializes the
// node, so the bridge has no native id when the attributes land. We
// stash them in _attributes (the existing path) and flush them once
// _nativeCreated flips true. Idempotent — safe to call from
// _ensureNative AND from the appendChild fast path.
function __replaySvgRectAttributes__(el) {
    if (!el || !el._nativeCreated || !el._attributes) return;
    if (el.tagName !== "RECT") return;
    var a = el._attributes;
    if (typeof setSvgRect === "function") {
        var x  = parseFloat(a.x  || "0");
        var y  = parseFloat(a.y  || "0");
        var w  = parseFloat(a.width  || "0");
        var h  = parseFloat(a.height || "0");
        if (x === x && y === y && w === w && h === h) {
            setSvgRect(el._id, x, y, w, h);
        }
    }
    if (a.fill !== undefined && typeof setSvgFill === "function") {
        setSvgFill(el._id, String(a.fill));
    }
    if (a.stroke !== undefined && typeof setSvgStroke === "function") {
        setSvgStroke(el._id, String(a.stroke));
    }
    var sw = a["stroke-width"];
    if (sw === undefined) sw = a.strokeWidth;
    if (sw !== undefined && typeof setSvgStrokeWidth === "function") {
        var psw = parseFloat(sw);
        if (psw === psw) setSvgStrokeWidth(el._id, psw);
    }
}

// pulp #1926 — replay <line> SVG attributes through the SvgLineWidget
// bridge. SvgLineWidget has no fill semantics — only stroke /
// stroke-width matter alongside the (x1,y1,x2,y2) endpoints.
function __replaySvgLineAttributes__(el) {
    if (!el || !el._nativeCreated || !el._attributes) return;
    if (el.tagName !== "LINE") return;
    var a = el._attributes;
    if (typeof setSvgLine === "function") {
        var x1 = parseFloat(a.x1 || "0");
        var y1 = parseFloat(a.y1 || "0");
        var x2 = parseFloat(a.x2 || "0");
        var y2 = parseFloat(a.y2 || "0");
        if (x1 === x1 && y1 === y1 && x2 === x2 && y2 === y2) {
            setSvgLine(el._id, x1, y1, x2, y2);
        }
    }
    if (typeof setSvgStroke === "function") {
        // SvgLineWidget's C++ default is has_stroke_=true (opaque
        // black, 1px), but SVG spec says <line> defaults to
        // stroke="none". Without explicit clearing, a JSX <line> that
        // omits `stroke` would paint an unwanted opaque-black stroke.
        // Explicitly drive the widget to the spec default when the
        // attribute is absent or empty (#1928 review).
        if (a.stroke !== undefined && String(a.stroke).length > 0) {
            setSvgStroke(el._id, String(a.stroke));
        } else {
            setSvgStroke(el._id, "none");
        }
    }
    var sw = a["stroke-width"];
    if (sw === undefined) sw = a.strokeWidth;
    if (sw !== undefined && typeof setSvgStrokeWidth === "function") {
        var psw = parseFloat(sw);
        if (psw === psw) setSvgStrokeWidth(el._id, psw);
    }
}

// pulp #1926 — replay <circle> attributes by synthesizing a `d` path
// from cx/cy/r and feeding through the SvgPathWidget bridge. Two SVG
// arc commands (each a half-circle, sweep flag = 0) draw the full
// circumference and Z closes the loop:
//   M (cx-r) cy
//   a r r 0 1 0 ( 2r) 0
//   a r r 0 1 0 (-2r) 0
//   Z
function __replaySvgCircleAttributes__(el) {
    if (!el || !el._nativeCreated || !el._attributes) return;
    if (el.tagName !== "CIRCLE") return;
    var a = el._attributes;
    var cx = parseFloat(a.cx || "0");
    var cy = parseFloat(a.cy || "0");
    var r  = parseFloat(a.r  || "0");
    if (cx !== cx || cy !== cy || r !== r || r <= 0) return;
    if (typeof setSvgPath === "function") {
        var d = "M " + (cx - r) + " " + cy +
                " a " + r + " " + r + " 0 1 0 " + (2 * r) + " 0" +
                " a " + r + " " + r + " 0 1 0 " + (-2 * r) + " 0 Z";
        setSvgPath(el._id, d);
    }
    if (a.fill !== undefined && typeof setSvgFill === "function") {
        setSvgFill(el._id, String(a.fill));
    }
    if (a.stroke !== undefined && typeof setSvgStroke === "function") {
        setSvgStroke(el._id, String(a.stroke));
    }
    var sw = a["stroke-width"];
    if (sw === undefined) sw = a.strokeWidth;
    if (sw !== undefined && typeof setSvgStrokeWidth === "function") {
        var psw = parseFloat(sw);
        if (psw === psw) setSvgStrokeWidth(el._id, psw);
    }
}

// pulp #1899 — replay <path> SVG attributes captured pre-mount through
// the SvgPathWidget bridge surface. React/JSX commits attributes before
// `appendChild` materializes the node, mirroring the aria / media-attr
// patterns above. Idempotent — safe to call from _ensureNative AND from
// the appendChild fast path. Also inherits the parent <svg>'s viewBox
// when set, matching the SVG spec (path coordinates live in the parent
// viewBox's coordinate space).
//
// Attribute name handling: HTML attribute names lowercase via
// setAttribute, but JSX often serializes camelCase `strokeWidth`. We
// accept either spelling so the host-config (React intrinsic) and the
// raw HTML/JSX path both work.
function __replaySvgPathAttributes__(el) {
    if (!el || !el._nativeCreated || !el._attributes) return;
    if (el.tagName !== "PATH") return;
    var a = el._attributes;

    // d — required for the path to paint at all.
    if (a.d !== undefined && typeof setSvgPath === "function") {
        setSvgPath(el._id, String(a.d));
    }
    // stroke — color string. "none" / "" clears the stroke.
    // pulp #1917 (Codex P1) — resolve `currentColor` against the CSS
    // color cascade before dispatch; the C++ widget doesn't parse the
    // literal token. Fallback is "black" per SVG spec.
    if (a.stroke !== undefined && typeof setSvgStroke === "function") {
        var strokeVal = String(a.stroke);
        if (strokeVal === "currentColor") {
            strokeVal = __resolveCurrentColor__(el);
        }
        setSvgStroke(el._id, strokeVal);
    }
    // stroke-width / strokeWidth — width in viewBox units.
    var sw = a["stroke-width"];
    if (sw === undefined) sw = a.strokeWidth;
    if (sw !== undefined && typeof setSvgStrokeWidth === "function") {
        var psw = parseFloat(sw);
        if (psw === psw) setSvgStrokeWidth(el._id, psw);
    }
    // fill — color string. "none" clears. Same currentColor handling as stroke.
    if (a.fill !== undefined && typeof setSvgFill === "function") {
        var fillVal = String(a.fill);
        if (fillVal === "currentColor") {
            fillVal = __resolveCurrentColor__(el);
        }
        setSvgFill(el._id, fillVal);
    }
    // viewBox — inherited from the parent <svg>. The SVG spec attaches
    // viewBox to the outer <svg>, but the SvgPathWidget needs the (w,h)
    // pair to scale path coordinates into widget bounds. Walk up until
    // we hit an <svg> ancestor (or the root) and lift its viewBox.
    if (typeof setSvgViewBox === "function") {
        var anc = el._parentElement;
        while (anc) {
            if (anc.tagName === "SVG") break;
            anc = anc._parentElement;
        }
        if (anc && anc._attributes) {
            var vb = anc._attributes.viewBox;
            if (typeof vb === "string") {
                var toks = vb.trim().split(/[\s,]+/).map(parseFloat);
                var clean = [];
                for (var ti = 0; ti < toks.length; ti++) {
                    if (toks[ti] === toks[ti]) clean.push(toks[ti]);
                }
                if (clean.length === 4) {
                    // SVG-spec form `min-x min-y w h` — bridge consumes w + h.
                    setSvgViewBox(el._id, clean[2], clean[3]);
                } else if (clean.length === 2) {
                    setSvgViewBox(el._id, clean[0], clean[1]);
                }
            }
        }
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
        // pulp DIVERGE→PASS sweep — wire the actual native widget
        // disabled-state through `setEnabled` so the View::enabled_
        // flag flips. Before, only the stylesheet flag changed
        // (`:disabled` selectors picked it up) but the underlying
        // widget kept handling pointer events / dispatching change.
        this._disabled = !!v;
        if (this._nativeCreated && typeof setEnabled === "function") {
            setEnabled(this._id, this._disabled ? 0 : 1);
        }
        this._reapplyStylesheets();
    }
});

// ── <dialog> showModal / close / show ─────────────────────────────────────
// pulp DIVERGE→PASS sweep — `<dialog>` previously created a hidden
// Panel and exposed no JS surface for opening / closing. The DOM
// `HTMLDialogElement` interface defines:
//   show()      — non-modal open (no backdrop, no input trap)
//   showModal() — modal open (backdrop, input trap, focus pull)
//   close()     — close the dialog and dispatch 'close'
// Pulp doesn't have a native modal-input-trap layer yet, so showModal
// degrades to show() + the open attribute. The `::backdrop` pseudo-
// element remains a roadmap item (separate paint-side concern). The
// behavioral gap was that `dialog.show()` was a TypeError; this slice
// exposes the methods so `addEventListener('close', ...)` consumers
// can drive the open state and round-trip the open attribute.

Element.prototype.show = function() {
    if (this.tagName !== "DIALOG") return;
    this._dialogOpen = true;
    this.setAttribute("open", "");
    if (this._nativeCreated && typeof setVisible === "function") {
        setVisible(this._id, true);
    }
};
Element.prototype.showModal = function() {
    if (this.tagName !== "DIALOG") return;
    // No native modal-input-trap yet — same effect as show() plus a
    // `_dialogModal` flag so consumers / future paint code can read
    // back whether modal semantics were requested.
    this._dialogModal = true;
    this.show();
};
Element.prototype.close = function(returnValue) {
    if (this.tagName !== "DIALOG") return;
    this._dialogOpen = false;
    this._dialogModal = false;
    this._dialogReturnValue = (returnValue !== undefined) ? String(returnValue) : "";
    this.removeAttribute("open");
    if (this._nativeCreated && typeof setVisible === "function") {
        setVisible(this._id, false);
    }
    var evt = (typeof Event === "function")
        ? new Event("close", { bubbles: false, cancelable: false })
        : { type: "close", target: this, bubbles: false, cancelable: false,
            _stopped: false, _defaultPrevented: false, _noBubble: true,
            stopPropagation: function() { this._stopped = true; },
            preventDefault: function() {} };
    this.dispatchEvent(evt);
};
Object.defineProperty(Element.prototype, "open", {
    // Reflects the `open` attribute for <dialog> AND <details>.
    get: function() {
        if (this.tagName === "DIALOG") return !!this._dialogOpen;
        if (this.tagName === "DETAILS") return !!this._detailsOpen;
        return this._attributes && (this._attributes.open !== undefined);
    },
    set: function(v) {
        var willOpen = !!v;
        if (this.tagName === "DIALOG") {
            if (willOpen) this.show(); else this.close();
            return;
        }
        if (this.tagName === "DETAILS") {
            this._detailsOpen = willOpen;
            if (willOpen) this.setAttribute("open", "");
            else this.removeAttribute("open");
            // Toggle visibility of children (other than the first
            // <summary>, which always shows). Roadmap: when <summary>
            // semantics land, hide children[1..] under closed details.
            this._reapplyStylesheets();
            var tevt = (typeof Event === "function")
                ? new Event("toggle", { bubbles: false, cancelable: false })
                : { type: "toggle", target: this, _stopped: false,
                    _defaultPrevented: false, _noBubble: true,
                    stopPropagation: function() { this._stopped = true; },
                    preventDefault: function() {} };
            this.dispatchEvent(tevt);
            return;
        }
    }
});
Object.defineProperty(Element.prototype, "returnValue", {
    get: function() { return this._dialogReturnValue || ""; },
    set: function(v) { this._dialogReturnValue = String(v || ""); }
});

// ── <label> for-attribute focus routing ─────────────────────────────────
// pulp DIVERGE→PASS sweep — clicking a <label> with a `for` attribute
// transfers focus / activation to the labeled element. We don't yet
// have a unified focus layer but the bridge has setFocus() — wire the
// click handler so the harness gap is closed at the JS layer.

Element.prototype._labelForRoutingInstalled = false;
Element.prototype._installLabelForRouting = function() {
    if (this.tagName !== "LABEL") return;
    if (this._labelForRoutingInstalled) return;
    this._labelForRoutingInstalled = true;
    var self = this;
    this.addEventListener("click", function(evt) {
        var forId = self.getAttribute("for");
        if (!forId || typeof document === "undefined") return;
        var target = document.getElementById(forId);
        if (!target) return;
        // Focus the labeled element. For checkbox/radio inputs the
        // standard behavior is to also toggle/activate; if we have a
        // bridge setFocus, prefer it; fall back to dispatchEvent.
        if (typeof setFocus === "function" && target._nativeCreated) {
            setFocus(target._id);
        }
        if (target.tagName === "INPUT" &&
            (target._type === "checkbox" || target._type === "radio")) {
            target.checked = !target._checked;
            var ievt = (typeof Event === "function")
                ? new Event("input", { bubbles: true })
                : { type: "input", target: target, _stopped: false,
                    _defaultPrevented: false, _noBubble: false,
                    stopPropagation: function() { this._stopped = true; },
                    preventDefault: function() {} };
            target.dispatchEvent(ievt);
        }
    });
};

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
    // pulp DIVERGE→PASS sweep — `<label for="x">` click routing. The
    // appendChild fast path goes through C++ `__domAppend` and skips
    // JS-side `_ensureNative`, so the install hook in _ensureNative
    // alone misses the React-style commit path. Install when the
    // `for` attribute lands on a LABEL element — idempotent because
    // `_installLabelForRouting` early-returns if already installed.
    else if (name === "for" && this.tagName === "LABEL") {
        this._installLabelForRouting();
    }
    // pulp #1658 — `<img src="...">` routes to setImageSource on the
    // ImageView native widget. ImageView::paint then decodes via
    // SkData::MakeFromFileName + SkImages::DeferredFromEncodedData
    // (Skia draw_image_from_file path). Idempotent: also replayed by
    // __replayMediaAttributes__ in the appendChild-after-setAttribute
    // path. Only file:// and bare-path forms are wired today; data:
    // URLs and http(s) fetch are deferred follow-ups (see #1658).
    else if (name === "src" && this.tagName === "IMG") {
        if (this._nativeCreated && typeof setImageSource === "function") {
            setImageSource(this._id, String(value));
        }
    }
    // pulp Wave 3 html.2 / #1476 — ARIA accessibility setters.
    // `aria-label` / `role` round-trip through native View::access_label_
    // and View::access_role_ so the macOS NSAccessibility bridge (and
    // the cross-platform AccessibilityTree snapshot) can read them.
    // Other ARIA attributes still store via _attributes for getAttribute
    // round-tripping; only the two with a Pulp storage slot are
    // forwarded today.  The bridge is a no-op when the widget hasn't
    // been created yet — a follow-up appendChild path replays the
    // attribute through the same code path.
    else if (name === "aria-label") {
        if (this._nativeCreated && typeof setAccessibilityLabel === "function") {
            setAccessibilityLabel(this._id, String(value));
        }
    }
    else if (name === "role") {
        if (this._nativeCreated && typeof setAccessibilityRole === "function") {
            setAccessibilityRole(this._id, String(value));
        }
    }
    // pulp #1737 — ARIA state attributes (aria-pressed/checked/disabled/
    // hidden) route through setAccessibilityState. Same pre-mount
    // capture pattern as aria-label / role above; __replayAriaAttributes__
    // flushes from _attributes when _nativeCreated flips. Other ARIA
    // attributes still store via _attributes for getAttribute round-trip
    // only — only the four with a Pulp View slot are forwarded.
    else if (name === "aria-pressed" || name === "aria-checked" ||
             name === "aria-disabled" || name === "aria-hidden") {
        if (this._nativeCreated && typeof setAccessibilityState === "function") {
            setAccessibilityState(this._id, name.slice(5), String(value));
        }
    }
    // pulp #1899 — `<path d=... stroke=... stroke-width=... fill=...>`
    // routes through the SvgPathWidget bridge. Idempotent: also replayed
    // by __replaySvgPathAttributes__ in the appendChild-after-setAttribute
    // path. We forward immediately if the widget already exists, and let
    // the replay flush from _attributes for the pre-mount case.
    else if (this.tagName === "PATH" &&
             (name === "d" || name === "stroke" || name === "fill" ||
              name === "stroke-width" || name === "strokeWidth")) {
        if (this._nativeCreated) {
            if (name === "d" && typeof setSvgPath === "function") {
                setSvgPath(this._id, String(value));
            } else if (name === "stroke" && typeof setSvgStroke === "function") {
                setSvgStroke(this._id, String(value));
            } else if (name === "fill" && typeof setSvgFill === "function") {
                setSvgFill(this._id, String(value));
            } else if ((name === "stroke-width" || name === "strokeWidth") &&
                       typeof setSvgStrokeWidth === "function") {
                var p = parseFloat(value);
                if (p === p) setSvgStrokeWidth(this._id, p);
            }
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
    // pulp #1641 followup — reset View::access_role_ / access_label_
    // when role / aria-label are removed. Without this the slot stayed
    // populated even after the author detached the attribute (a user-
    // observable bug for assistive tech that reads stale state).
    else if (name === "role" && this._nativeCreated &&
             typeof setAccessibilityRole === "function") {
        setAccessibilityRole(this._id, "");
    }
    else if (name === "aria-label" && this._nativeCreated &&
             typeof setAccessibilityLabel === "function") {
        setAccessibilityLabel(this._id, "");
    }
    // pulp #1737 — same reset semantics for ARIA state attributes.
    // Empty string clears the View::access_*_ slot.
    else if ((name === "aria-pressed" || name === "aria-checked" ||
              name === "aria-disabled" || name === "aria-hidden") &&
             this._nativeCreated &&
             typeof setAccessibilityState === "function") {
        setAccessibilityState(this._id, name.slice(5), "");
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

// Standard DOM `Node.contains(other)` — returns true if `other` is this
// element OR a descendant. Walks `_parentElement` upward from `other`
// until we hit `this`, the root, or a cycle. Required for click-outside
// detection (the `ref.current.contains(e.target)` pattern in React
// portals / ContextMenus). Pre-#1859 audit of Spectr's working bundle
// found this missing from the modular Element shim, would have thrown
// `TypeError: ref.current.contains is not a function` on
// ContextMenu dismiss after #1859 lands the DOM-shim path.
//
// Treats non-Element arguments (text nodes, null, etc.) as `false` —
// the contract is loose because most consumers feed e.target which is
// always an Element in our event system.
Element.prototype.contains = function(other) {
    if (other == null) return false;
    if (other === this) return true;
    // Guard against pathological cycles even though _parentElement
    // walks should be acyclic in practice.
    var seen = 0;
    var node = other._parentElement;
    while (node) {
        if (node === this) return true;
        if (++seen > 4096) return false;
        node = node._parentElement;
    }
    return false;
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
Event.prototype.stopPropagation = function() { this._stopped = true; };
Event.prototype.stopImmediatePropagation = function() { this._stopped = true; };
Event.prototype.preventDefault = function() {
    if (this.cancelable) this._defaultPrevented = true;
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
    } else if (tag === "path") {
        // pulp #1899 — `<path>` reparent path mirrors _ensureNative.
        // SvgPath attribute replay runs at the end of this function.
        if (typeof createSvgPath === "function") {
            createSvgPath(id, parentId);
        } else {
            createCol(id, parentId);
        }
    } else if (tag === "button") {
        createToggleButton(id, parentId);
    } else if (tag === "input") {
        var t = child._type || "text";
        // pulp #1899 — horizontal-by-default range slider. See
        // __resolveRangeOrientation__ in _ensureNative.
        if (t === "range") createFader(id, __resolveRangeOrientation__(child), parentId);
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
        // pulp #1658 — see initial-mount img branch above. _reparentNative
        // here covers the late-mount case (e.g. detached DOM that's later
        // appended); same createImage routing so the bridge fn matches
        // the harness oracle's `expected_bridge_calls: ["createImage"]`.
        createImage(id, parentId);
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
    // pulp Wave 3 html.2 / #1476 — replay ARIA attributes after reparent.
    if (typeof __replayAriaAttributes__ === "function") {
        __replayAriaAttributes__(child);
    }
    // pulp #1899 — replay SvgPath attributes after reparent so the
    // SvgPathWidget bridge sees d / stroke / stroke-width / fill /
    // viewBox even if the path was constructed before mount.
    if (typeof __replaySvgPathAttributes__ === "function") {
        __replaySvgPathAttributes__(child);
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
