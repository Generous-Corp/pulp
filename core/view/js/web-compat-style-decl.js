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
            else if (resolved === "flex" || resolved === "block") { setVisible(id, true); }
            else if (resolved === "grid") { /* grid mode set via gridTemplateColumns */ }
            break;
        case "flexDirection":
            setFlex(id, "direction", resolved === "row" ? "row" : "col");
            break;
        case "flexWrap":
            setFlex(id, "flex_wrap", resolved === "wrap" ? 1 : 0);
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

        // Dimensions
        case "width": {
            var w = parseCSSLength(resolved);
            if (w) setFlex(id, "width", w.value);
            break;
        }
        case "height": {
            var h = parseCSSLength(resolved);
            if (h) setFlex(id, "height", h.value);
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

        // Touch behavior (W3C touch-action)
        case "touch-action":
        case "touchAction":
            // Store on element for pointer event handling
            // Values: auto, none, pan-x, pan-y, pinch-zoom, manipulation
            this._touchAction = resolved;
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
        case "top": { var tv = parseCSSLength(resolved); if (tv) setTop(id, tv.value); break; }
        case "right": { var rv = parseCSSLength(resolved); if (rv) setRight(id, rv.value); break; }
        case "bottom": { var bv = parseCSSLength(resolved); if (bv) setBottom(id, bv.value); break; }
        case "left": { var lv = parseCSSLength(resolved); if (lv) setLeft(id, lv.value); break; }

        // z-index
        case "zIndex":
            setZIndex(id, parseInt(resolved) || 0);
            break;

        // Box shadow: "2px 4px 8px rgba(0,0,0,0.3)"
        case "boxShadow": {
            if (resolved === "none") { /* clear shadow */ break; }
            var sm = resolved.match(/(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px(?:\s+([\d.]+)px)?\s+(.*)/);
            if (sm) {
                var sc = parseCSSColor(sm[5].trim());
                setBoxShadow(id, parseFloat(sm[1]), parseFloat(sm[2]),
                            parseFloat(sm[3]), parseFloat(sm[4] || 0), sc || sm[5].trim());
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

        // ── P1: New CSS properties ──────────────────────────────────────

        // aspect-ratio: "16/9" or "1"
        case "aspectRatio": {
            var arParts = resolved.split("/");
            var ratio = parseFloat(arParts[0]) || 1;
            if (arParts[1]) ratio /= parseFloat(arParts[1]) || 1;
            setFlex(id, "aspect_ratio", ratio);
            break;
        }

        // visibility: "hidden" vs display:none — hidden preserves layout space
        case "visibility":
            if (typeof setVisibility === "function") setVisibility(id, resolved);
            else if (resolved === "hidden") setOpacity(id, 0);
            else setOpacity(id, 1);
            break;

        // outline: "2px solid blue"
        case "outline": {
            var op = resolved.match(/([\d.]+)px\s+\w+\s+(.+)/);
            if (op) {
                var oc = parseCSSColor(op[2].trim());
                if (typeof setOutline === "function") setOutline(id, parseFloat(op[1]), oc || op[2].trim());
            }
            break;
        }
        case "outlineWidth": {
            var ow = parseCSSLength(resolved);
            if (ow && typeof setOutline === "function") setOutline(id, ow.value, "");
            break;
        }
        case "outlineColor": {
            var occ = parseCSSColor(resolved);
            if (occ && typeof setOutline === "function") setOutline(id, 0, occ);
            break;
        }

        // white-space: "nowrap", "pre", "normal"
        case "whiteSpace":
            if (typeof setWhiteSpace === "function") setWhiteSpace(id, resolved);
            break;

        // word-break / overflow-wrap
        case "wordBreak":
        case "overflowWrap":
        case "wordWrap":
            if (typeof setWordBreak === "function") setWordBreak(id, resolved);
            break;

        // text-shadow: "2px 2px 4px rgba(0,0,0,0.5)"
        case "textShadow": {
            var tsm = resolved.match(/(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px\s+(.*)/);
            if (tsm && typeof setTextShadow === "function") {
                var tsc = parseCSSColor(tsm[4].trim());
                setTextShadow(id, parseFloat(tsm[1]), parseFloat(tsm[2]), parseFloat(tsm[3]), tsc || tsm[4].trim());
            }
            break;
        }

        // user-select: "none", "text", "all"
        case "userSelect":
            if (typeof setUserSelect === "function") setUserSelect(id, resolved);
            break;

        // pointer-events: "none", "auto"
        case "pointerEvents":
            if (typeof setPointerEvents === "function") setPointerEvents(id, resolved);
            break;

        // font-family
        case "fontFamily":
            if (typeof setFontFamily === "function") setFontFamily(id, resolved.replace(/['"]/g, ""));
            break;

        // background-size: "cover", "contain", "100px 200px"
        case "backgroundSize":
            if (typeof setBackgroundSize === "function") setBackgroundSize(id, resolved);
            break;

        // background-position: "center", "top left", "50% 50%"
        case "backgroundPosition":
            if (typeof setBackgroundPosition === "function") setBackgroundPosition(id, resolved);
            break;

        // align-content (multi-line flex cross-axis)
        case "alignContent":
            setFlex(id, "align_content", _cssToFlex(resolved));
            break;
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
    "justifyContent", "alignItems", "alignSelf", "alignContent", "order",
    "gap", "rowGap", "columnGap",
    "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
    "aspectRatio",
    "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
    "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "backgroundColor", "color",
    "fontSize", "fontWeight", "fontStyle", "fontFamily", "letterSpacing", "lineHeight",
    "textAlign", "textTransform", "textDecoration", "textOverflow", "textShadow",
    "whiteSpace", "wordBreak", "overflowWrap", "wordWrap",
    "border", "borderColor", "borderWidth", "borderRadius",
    "outline", "outlineWidth", "outlineColor",
    "opacity", "overflow", "cursor", "visibility",
    "userSelect", "pointerEvents",
    "transform", "transformOrigin",
    "transition", "transitionDuration",
    "position", "top", "right", "bottom", "left", "zIndex",
    "boxShadow", "filter", "background", "backgroundImage",
    "backgroundSize", "backgroundPosition",
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
        var parsed = parseCSSLength(value);
        if (parsed) {
            setMotionToken(tokenName, parsed.value);
        } else {
            // Color token? Store as a theme color override
            var color = parseCSSColor(value);
            if (color) {
                // Use applyTokenDiff for color tokens
                applyTokenDiff('{"colors":{"' + tokenName + '":"' + color + '"}}');
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
