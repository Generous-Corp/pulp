#pragma once
// Auto-generated from JS prelude files — do not edit
namespace pulp::view::preludes {

static const char* css_colors =
R"__JS__(// css-colors.js — All 148 named CSS colors + transparent/currentColor
// Loaded as a prelude before web-compat.js

var __cssColors__ = {
    aliceblue: "#f0f8ff", antiquewhite: "#faebd7", aqua: "#00ffff",
    aquamarine: "#7fffd4", azure: "#f0ffff", beige: "#f5f5dc",
    bisque: "#ffe4c4", black: "#000000", blanchedalmond: "#ffebcd",
    blue: "#0000ff", blueviolet: "#8a2be2", brown: "#a52a2a",
    burlywood: "#deb887", cadetblue: "#5f9ea0", chartreuse: "#7fff00",
    chocolate: "#d2691e", coral: "#ff7f50", cornflowerblue: "#6495ed",
    cornsilk: "#fff8dc", crimson: "#dc143c", cyan: "#00ffff",
    darkblue: "#00008b", darkcyan: "#008b8b", darkgoldenrod: "#b8860b",
    darkgray: "#a9a9a9", darkgreen: "#006400", darkgrey: "#a9a9a9",
    darkkhaki: "#bdb76b", darkmagenta: "#8b008b", darkolivegreen: "#556b2f",
    darkorange: "#ff8c00", darkorchid: "#9932cc", darkred: "#8b0000",
    darksalmon: "#e9967a", darkseagreen: "#8fbc8f", darkslateblue: "#483d8b",
    darkslategray: "#2f4f4f", darkslategrey: "#2f4f4f", darkturquoise: "#00ced1",
    darkviolet: "#9400d3", deeppink: "#ff1493", deepskyblue: "#00bfff",
    dimgray: "#696969", dimgrey: "#696969", dodgerblue: "#1e90ff",
    firebrick: "#b22222", floralwhite: "#fffaf0", forestgreen: "#228b22",
    fuchsia: "#ff00ff", gainsboro: "#dcdcdc", ghostwhite: "#f8f8ff",
    gold: "#ffd700", goldenrod: "#daa520", gray: "#808080",
    green: "#008000", greenyellow: "#adff2f", grey: "#808080",
    honeydew: "#f0fff0", hotpink: "#ff69b4", indianred: "#cd5c5c",
    indigo: "#4b0082", ivory: "#fffff0", khaki: "#f0e68c",
    lavender: "#e6e6fa", lavenderblush: "#fff0f5", lawngreen: "#7cfc00",
    lemonchiffon: "#fffacd", lightblue: "#add8e6", lightcoral: "#f08080",
    lightcyan: "#e0ffff", lightgoldenrodyellow: "#fafad2", lightgray: "#d3d3d3",
    lightgreen: "#90ee90", lightgrey: "#d3d3d3", lightpink: "#ffb6c1",
    lightsalmon: "#ffa07a", lightseagreen: "#20b2aa", lightskyblue: "#87cefa",
    lightslategray: "#778899", lightslategrey: "#778899", lightsteelblue: "#b0c4de",
    lightyellow: "#ffffe0", lime: "#00ff00", limegreen: "#32cd32",
    linen: "#faf0e6", magenta: "#ff00ff", maroon: "#800000",
    mediumaquamarine: "#66cdaa", mediumblue: "#0000cd", mediumorchid: "#ba55d3",
    mediumpurple: "#9370db", mediumseagreen: "#3cb371", mediumslateblue: "#7b68ee",
    mediumspringgreen: "#00fa9a", mediumturquoise: "#48d1cc", mediumvioletred: "#c71585",
    midnightblue: "#191970", mintcream: "#f5fffa", mistyrose: "#ffe4e1",
    moccasin: "#ffe4b5", navajowhite: "#ffdead", navy: "#000080",
    oldlace: "#fdf5e6", olive: "#808000", olivedrab: "#6b8e23",
    orange: "#ffa500", orangered: "#ff4500", orchid: "#da70d6",
    palegoldenrod: "#eee8aa", palegreen: "#98fb98", paleturquoise: "#afeeee",
    palevioletred: "#db7093", papayawhip: "#ffefd5", peachpuff: "#ffdab9",
    peru: "#cd853f", pink: "#ffc0cb", plum: "#dda0dd",
    powderblue: "#b0e0e6", purple: "#800080", rebeccapurple: "#663399",
    red: "#ff0000", rosybrown: "#bc8f8f", royalblue: "#4169e1",
    saddlebrown: "#8b4513", salmon: "#fa8072", sandybrown: "#f4a460",
    seagreen: "#2e8b57", seashell: "#fff5ee", sienna: "#a0522d",
    silver: "#c0c0c0", skyblue: "#87ceeb", slateblue: "#6a5acd",
    slategray: "#708090", slategrey: "#708090", snow: "#fffafa",
    springgreen: "#00ff7f", steelblue: "#4682b4", tan: "#d2b48c",
    teal: "#008080", thistle: "#d8bfd8", tomato: "#ff6347",
    turquoise: "#40e0d0", violet: "#ee82ee", wheat: "#f5deb3",
    white: "#ffffff", whitesmoke: "#f5f5f5", yellow: "#ffff00",
    yellowgreen: "#9acd32",
    transparent: "transparent",
    currentcolor: "currentColor"
};
)__JS__"
;

static const char* css_parser =
R"__JS__(// css-parser.js — CSS value parsing utilities
// Loaded as a prelude before web-compat.js
// Depends on: css-colors.js (__cssColors__)

// Parse a CSS length value: "12px", "1.5em", "50%", "auto"
function parseCSSLength(str) {
    if (str === undefined || str === null || str === "") return null;
    str = String(str).trim();
    if (str === "auto") return { value: 0, unit: "auto" };
    if (str === "0") return { value: 0, unit: "px" };

    var match = str.match(/^(-?[\d.]+)(px|em|rem|%|vw|vh|vmin|vmax)?$/);
    if (match) {
        return { value: parseFloat(match[1]), unit: match[2] || "px" };
    }
    // Bare number (treat as px)
    var n = parseFloat(str);
    if (!isNaN(n)) return { value: n, unit: "px" };
    return null;
}

// Parse a CSS color string to a hex string the bridge understands
function parseCSSColor(str) {
    if (str === undefined || str === null || str === "") return null;
    str = String(str).trim().toLowerCase();

    // transparent
    if (str === "transparent") return "#00000000";

    // Hex passthrough
    if (str[0] === "#") return str;

    // Named color
    if (__cssColors__[str]) {
        var v = __cssColors__[str];
        if (v === "transparent") return "#00000000";
        if (v === "currentColor") return null; // resolved by theme
        return v;
    }

    // rgb(r, g, b) / rgba(r, g, b, a)
    var rgbMatch = str.match(/^rgba?\(\s*([\d.]+)[,%\s]+([\d.]+)[,%\s]+([\d.]+)(?:[,/\s]+([\d.]+%?))?\s*\)$/);
    if (rgbMatch) {
        var r = Math.round(Math.min(255, Math.max(0, parseFloat(rgbMatch[1]))));
        var g = Math.round(Math.min(255, Math.max(0, parseFloat(rgbMatch[2]))));
        var b = Math.round(Math.min(255, Math.max(0, parseFloat(rgbMatch[3]))));
        var a = 255;
        if (rgbMatch[4] !== undefined) {
            var av = rgbMatch[4];
            if (av.indexOf("%") >= 0) a = Math.round(parseFloat(av) * 2.55);
            else {
                var af = parseFloat(av);
                a = af <= 1 ? Math.round(af * 255) : Math.round(af);
            }
        }
        return "#" + _hex2(r) + _hex2(g) + _hex2(b) + (a < 255 ? _hex2(a) : "");
    }

    // hsl(h, s%, l%) / hsla(h, s%, l%, a)
    var hslMatch = str.match(/^hsla?\(\s*([\d.]+)[,\s]+([\d.]+)%[,\s]+([\d.]+)%(?:[,/\s]+([\d.]+%?))?\s*\)$/);
    if (hslMatch) {
        var h = (parseFloat(hslMatch[1]) % 360) / 360;
        var s = parseFloat(hslMatch[2]) / 100;
        var l = parseFloat(hslMatch[3]) / 100;
        var rgb = _hslToRgb(h, s, l);
        var a2 = 255;
        if (hslMatch[4] !== undefined) {
            var av2 = hslMatch[4];
            if (av2.indexOf("%") >= 0) a2 = Math.round(parseFloat(av2) * 2.55);
            else {
                var af2 = parseFloat(av2);
                a2 = af2 <= 1 ? Math.round(af2 * 255) : Math.round(af2);
            }
        }
        return "#" + _hex2(rgb[0]) + _hex2(rgb[1]) + _hex2(rgb[2]) + (a2 < 255 ? _hex2(a2) : "");
    }

    return null;
}

function _hex2(n) {
    var s = Math.round(Math.min(255, Math.max(0, n))).toString(16);
    return s.length < 2 ? "0" + s : s;
}

function _hslToRgb(h, s, l) {
    var r, g, b;
    if (s === 0) {
        r = g = b = l;
    } else {
        var q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        var p = 2 * l - q;
        r = _hue2rgb(p, q, h + 1/3);
        g = _hue2rgb(p, q, h);
        b = _hue2rgb(p, q, h - 1/3);
    }
    return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
}

function _hue2rgb(p, q, t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1/6) return p + (q - p) * 6 * t;
    if (t < 1/2) return q;
    if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
    return p;
}

// Expand CSS shorthand: "10px" -> [10,10,10,10], "10px 20px" -> [10,20,10,20],
// "10px 20px 30px" -> [10,20,30,20], "10px 20px 30px 40px" -> [10,20,30,40]
function expandShorthand(str) {
    if (str === undefined || str === null) return [0, 0, 0, 0];
    var parts = String(str).trim().split(/\s+/);
    var vals = [];
    for (var i = 0; i < parts.length; i++) {
        var p = parseCSSLength(parts[i]);
        vals.push(p ? p.value : 0);
    }
    if (vals.length === 1) return [vals[0], vals[0], vals[0], vals[0]];
    if (vals.length === 2) return [vals[0], vals[1], vals[0], vals[1]];
    if (vals.length === 3) return [vals[0], vals[1], vals[2], vals[1]];
    return [vals[0] || 0, vals[1] || 0, vals[2] || 0, vals[3] || 0];
}

// Parse CSS transform string: "scale(1.1) rotate(45deg) translate(10px, 20px)"
function parseTransform(str) {
    if (!str) return [];
    var result = [];
    var re = /(\w+)\(([^)]*)\)/g;
    var m;
    while ((m = re.exec(str)) !== null) {
        var fn = m[1];
        var rawArgs = m[2].split(",").map(function(s) { return s.trim(); });
        var args = rawArgs.map(function(a) {
            if (a.indexOf("deg") >= 0) return parseFloat(a);
            var l = parseCSSLength(a);
            return l ? l.value : parseFloat(a) || 0;
        });
        result.push({ fn: fn, args: args });
    }
    return result;
}

// Parse CSS transition shorthand: "all 0.3s ease-out 0.1s"
function parseTransition(str) {
    if (!str) return { property: "all", duration: 0, easing: "ease", delay: 0 };
    var parts = String(str).trim().split(/\s+/);
    var result = { property: "all", duration: 0, easing: "ease", delay: 0 };
    var timeIdx = 0;
    for (var i = 0; i < parts.length; i++) {
        var p = parts[i];
        if (p.match(/^[\d.]+m?s$/)) {
            var val = parseFloat(p);
            if (p.indexOf("ms") >= 0) val /= 1000;
            if (timeIdx === 0) { result.duration = val; timeIdx++; }
            else result.delay = val;
        } else if (p.match(/^(ease|ease-in|ease-out|ease-in-out|linear|cubic-bezier)/) ) {
            result.easing = p;
        } else {
            result.property = p;
        }
    }
    return result;
}

// Resolve var(--name) or var(--name, fallback) against theme tokens
function _resolveVar(str) {
    if (!str || typeof str !== "string") return str;
    return str.replace(/var\(\s*--([^,)]+)(?:\s*,\s*([^)]+))?\s*\)/g, function(_, name, fallback) {
        var val = getMotionToken(name.trim());
        if (val !== 0 && val !== undefined) return String(val);
        if (fallback !== undefined) return fallback.trim();
        return "0";
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// Unit resolution — convert parsed {value, unit} to pixels given context
// ═══════════════════════════════════════════════════════════════════════════════

// Context: { parentWidth, parentHeight, fontSize, rootFontSize, viewportW, viewportH }
function resolveLength(parsed, ctx) {
    if (!parsed) return 0;
    if (parsed.unit === "auto") return 0; // caller handles auto specially
    if (parsed.unit === "px" || !parsed.unit) return parsed.value;
    if (!ctx) return parsed.value; // fallback: treat as px

    switch (parsed.unit) {
        case "em":   return parsed.value * (ctx.fontSize || 14);
        case "rem":  return parsed.value * (ctx.rootFontSize || 14);
        case "%":    return parsed.value / 100 * (ctx.parentSize || ctx.parentWidth || 0);
        case "vw":   return parsed.value / 100 * (ctx.viewportW || 800);
        case "vh":   return parsed.value / 100 * (ctx.viewportH || 600);
        case "vmin": return parsed.value / 100 * Math.min(ctx.viewportW || 800, ctx.viewportH || 600);
        case "vmax": return parsed.value / 100 * Math.max(ctx.viewportW || 800, ctx.viewportH || 600);
        case "ch":   return parsed.value * (ctx.fontSize || 14) * 0.5; // approximate
        default:     return parsed.value;
    }
}

// Resolve a CSS length string to px in one call
function resolveCSSLength(str, ctx) {
    if (!str) return 0;
    str = String(str).trim();
    // Check for calc/min/max/clamp first
    if (str.indexOf("calc(") === 0 || str.indexOf("min(") === 0 ||
        str.indexOf("max(") === 0 || str.indexOf("clamp(") === 0) {
        return evaluateCalc(str, ctx);
    }
    var parsed = parseCSSLength(str);
    if (!parsed) return 0;
    return resolveLength(parsed, ctx);
}

// ═══════════════════════════════════════════════════════════════════════════════
// calc() / min() / max() / clamp() expression evaluator
// ═══════════════════════════════════════════════════════════════════════════════

function evaluateCalc(expr, ctx) {
    if (!expr) return 0;
    expr = String(expr).trim();

    // Strip outer function wrapper
    if (expr.indexOf("calc(") === 0) expr = expr.slice(5, -1);

    // Handle min(a, b, ...)
    var minMatch = expr.match(/^min\((.+)\)$/);
    if (minMatch) {
        var parts = _splitCalcArgs(minMatch[1]);
        var vals = parts.map(function(p) { return evaluateCalc(p.trim(), ctx); });
        return Math.min.apply(null, vals);
    }

    // Handle max(a, b, ...)
    var maxMatch = expr.match(/^max\((.+)\)$/);
    if (maxMatch) {
        var parts2 = _splitCalcArgs(maxMatch[1]);
        var vals2 = parts2.map(function(p) { return evaluateCalc(p.trim(), ctx); });
        return Math.max.apply(null, vals2);
    }

    // Handle clamp(min, preferred, max)
    var clampMatch = expr.match(/^clamp\((.+)\)$/);
    if (clampMatch) {
        var parts3 = _splitCalcArgs(clampMatch[1]);
        if (parts3.length >= 3) {
            var lo = evaluateCalc(parts3[0].trim(), ctx);
            var pref = evaluateCalc(parts3[1].trim(), ctx);
            var hi = evaluateCalc(parts3[2].trim(), ctx);
            return Math.min(Math.max(pref, lo), hi);
        }
        return 0;
    }

    // Evaluate arithmetic expression: supports +, -, *, /
    // First resolve nested function calls
    expr = expr.replace(/(calc|min|max|clamp)\([^)]*\)/g, function(match) {
        return String(evaluateCalc(match, ctx));
    });

    // Tokenize: numbers with units, operators
    var tokens = [];
    var re = /(-?[\d.]+(?:px|em|rem|%|vw|vh|vmin|vmax|ch)?)|([+\-*/])/g;
    var m;
    while ((m = re.exec(expr)) !== null) {
        if (m[1]) {
            var parsed = parseCSSLength(m[1]);
            tokens.push({ type: "num", value: parsed ? resolveLength(parsed, ctx) : parseFloat(m[1]) || 0 });
        } else if (m[2]) {
            tokens.push({ type: "op", value: m[2] });
        }
    }

    if (tokens.length === 0) return 0;

    // Evaluate: * and / first, then + and -
    // Pass 1: multiply and divide
    var simplified = [tokens[0]];
    for (var i = 1; i < tokens.length - 1; i += 2) {
        var op = tokens[i];
        var next = tokens[i + 1];
        if (!op || !next) break;
        if (op.value === "*") {
            simplified[simplified.length - 1] = { type: "num", value: simplified[simplified.length - 1].value * next.value };
        } else if (op.value === "/") {
            simplified[simplified.length - 1] = { type: "num", value: next.value !== 0 ? simplified[simplified.length - 1].value / next.value : 0 };
        } else {
            simplified.push(op, next);
        }
    }

    // Pass 2: add and subtract
    var result = simplified[0] ? simplified[0].value : 0;
    for (var j = 1; j < simplified.length - 1; j += 2) {
        var op2 = simplified[j];
        var next2 = simplified[j + 1];
        if (!op2 || !next2) break;
        if (op2.value === "+") result += next2.value;
        else if (op2.value === "-") re)__JS__"
R"__JS__(sult -= next2.value;
    }

    return result;
}

// Split comma-separated args respecting nested parentheses
function _splitCalcArgs(str) {
    var args = [], depth = 0, current = "";
    for (var i = 0; i < str.length; i++) {
        var c = str[i];
        if (c === "(") depth++;
        else if (c === ")") depth--;
        if (c === "," && depth === 0) {
            args.push(current);
            current = "";
        } else {
            current += c;
        }
    }
    if (current) args.push(current);
    return args;
}

// ═══════════════════════════════════════════════════════════════════════════════
// matchMedia — responsive breakpoint queries
// ═══════════════════════════════════════════════════════════════════════════════

function _matchMediaQuery(query) {
    // Parse: "(min-width: 600px)", "(max-width: 400px)", "(orientation: landscape)"
    var rootW = (typeof getRootSize === "function") ? getRootSize().width : 800;
    var rootH = (typeof getRootSize === "function") ? getRootSize().height : 600;

    var minW = query.match(/min-width:\s*([\d.]+)px/);
    if (minW && rootW < parseFloat(minW[1])) return false;

    var maxW = query.match(/max-width:\s*([\d.]+)px/);
    if (maxW && rootW > parseFloat(maxW[1])) return false;

    var minH = query.match(/min-height:\s*([\d.]+)px/);
    if (minH && rootH < parseFloat(minH[1])) return false;

    var maxH = query.match(/max-height:\s*([\d.]+)px/);
    if (maxH && rootH > parseFloat(maxH[1])) return false;

    var orient = query.match(/orientation:\s*(landscape|portrait)/);
    if (orient) {
        if (orient[1] === "landscape" && rootW <= rootH) return false;
        if (orient[1] === "portrait" && rootW > rootH) return false;
    }

    return true; // If no conditions matched, it passes
}
)__JS__"
;

static const char* web_compat_element =
R"__JS__(// web-compat.js — Web-native authoring layer for Pulp
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
        var siblings = this._pare)__JS__"
R"__JS__(ntElement._children;
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
        var listeners2 = __eventListeners__[path[k]._id] && __ev)__JS__"
R"__JS__(entListeners__[path[k]._id][event.type];
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
)__JS__"
;

static const char* web_compat_canvas =
R"__JS__(// ═══════════════════════════════════════════════════════════════════════════════
// HTMLCanvasElement + CanvasRenderingContext2D
// ═══════════════════════════════════════════════════════════════════════════════

function CanvasRenderingContext2D(canvasEl) {
    this.canvas = canvasEl;
    this._id = canvasEl._id;
    this.fillStyle = "#000000";
    this.strokeStyle = "#000000";
    this.lineWidth = 1;
    this.font = "14px Inter";
}

CanvasRenderingContext2D.prototype._applyFillStyle = function() {
    if (typeof canvasSetFillColor === "function") canvasSetFillColor(this._id, this.fillStyle);
};

CanvasRenderingContext2D.prototype._applyStrokeStyle = function() {
    if (typeof canvasSetStrokeColor === "function") canvasSetStrokeColor(this._id, this.strokeStyle);
    if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, this.lineWidth);
};

CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {
    this._applyFillStyle();
    if (typeof canvasFillRect === "function") canvasFillRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.strokeRect = function(x, y, w, h) {
    this._applyStrokeStyle();
    if (typeof canvasStrokeRect === "function") canvasStrokeRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.clearRect = function(x, y, w, h) {
    if (typeof canvasClearRect === "function") canvasClearRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.beginPath = function() {
    if (typeof canvasBeginPath === "function") canvasBeginPath(this._id);
};

CanvasRenderingContext2D.prototype.moveTo = function(x, y) {
    if (typeof canvasMoveTo === "function") canvasMoveTo(this._id, x, y);
};

CanvasRenderingContext2D.prototype.lineTo = function(x, y) {
    if (typeof canvasLineTo === "function") canvasLineTo(this._id, x, y);
};

CanvasRenderingContext2D.prototype.closePath = function() {
    if (typeof canvasClosePath === "function") canvasClosePath(this._id);
};

CanvasRenderingContext2D.prototype.fill = function() {
    this._applyFillStyle();
    if (typeof canvasFillPath === "function") canvasFillPath(this._id);
};

CanvasRenderingContext2D.prototype.stroke = function() {
    this._applyStrokeStyle();
    if (typeof canvasStrokePath === "function") canvasStrokePath(this._id);
};

function __ensurePulpGpuHelpers() {
    if (typeof window === "undefined" || !window.pulp || !window.pulp.gpu) return;
    if (window.pulp.gpu._nativeHelpersInstalled) return;

    var originalCreateMockDevice = window.pulp.gpu.createMockDevice;
    window.pulp.gpu.createMockDevice = function(adapter, descriptor) {
        adapter = adapter && adapter._objectName === "GPUAdapter" ? adapter : window.pulp.gpu.createMockAdapter();
        descriptor = descriptor || {};
        if (adapter._nativeBridge && typeof __describeNativeDeviceImpl === "function") {
            return __createMockGPUDevice(adapter, descriptor, __describeNativeDeviceImpl(descriptor) || {});
        }
        return originalCreateMockDevice.call(window.pulp.gpu, adapter, descriptor);
    };
    window.pulp.gpu.createNativeDevice = function(adapter, descriptor) {
        adapter = adapter && adapter._nativeBridge ? adapter : window.pulp.gpu.createNativeAdapter();
        if (!adapter) return null;
        descriptor = descriptor || {};
        if (typeof __describeNativeDeviceImpl === "function") {
            return __createMockGPUDevice(adapter, descriptor, __describeNativeDeviceImpl(descriptor) || {});
        }
        return __createMockGPUDevice(adapter, descriptor, { nativeBridge: true });
    };
    window.pulp.gpu._nativeHelpersInstalled = true;
    __installNativeGpuCommandAugmentation();
}

function __installNativeGpuCommandAugmentation() {
    if (typeof __createMockGPURenderPassEncoder !== "function" ||
        typeof __createMockGPURenderPipeline !== "function" ||
        typeof __createMockGPUQueue !== "function" ||
        typeof __createMockGPUDevice !== "function") {
        return;
    }
    if (__installNativeGpuCommandAugmentation._installed) return;

    var originalCreateMockGPURenderPassEncoder = __createMockGPURenderPassEncoder;
    var originalCreateMockGPURenderPipeline = __createMockGPURenderPipeline;
    var originalCreateMockGPUQueue = __createMockGPUQueue;
    var originalCreateMockGPUDevice = __createMockGPUDevice;

    function cloneBufferBytes(binding) {
        if (!binding || !binding.buffer || !binding.buffer._bytes) return [];
        var source = binding.buffer._bytes;
        var begin = binding.offset == null ? 0 : binding.offset;
        var end = binding.size == null ? source.length : begin + binding.size;
        if (begin < 0) begin = 0;
        if (end < begin) end = begin;
        return Array.from(source.slice(begin, end));
    }

    function findLayoutEntry(layoutEntries, binding) {
        if (!layoutEntries || typeof layoutEntries.length !== "number") return null;
        for (var i = 0; i < layoutEntries.length; ++i) {
            var entry = layoutEntries[i];
            if (entry && entry.binding === binding) return entry;
        }
        return null;
    }

    function shaderUsesBinding(code, groupIndex, binding) {
        if (!code) return false;
        var bindingThenGroup = new RegExp("@binding\\s*\\(\\s*" + binding + "\\s*\\)\\s*@group\\s*\\(\\s*" + groupIndex + "\\s*\\)");
        var groupThenBinding = new RegExp("@group\\s*\\(\\s*" + groupIndex + "\\s*\\)\\s*@binding\\s*\\(\\s*" + binding + "\\s*\\)");
        return bindingThenGroup.test(code) || groupThenBinding.test(code);
    }

    function inferVisibilityFromShaders(groupIndex, binding, vertexCode, fragmentCode) {
        var visibility = 0;
        if (shaderUsesBinding(vertexCode, groupIndex, binding)) {
            visibility |= (typeof GPUShaderStage !== "undefined") ? GPUShaderStage.VERTEX : 0x1;
        }
        if (shaderUsesBinding(fragmentCode, groupIndex, binding)) {
            visibility |= (typeof GPUShaderStage !== "undefined") ? GPUShaderStage.FRAGMENT : 0x2;
        }
        return visibility || ((typeof GPUShaderStage !== "undefined") ? (GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT) : 0x3);
    }

    function serializeBindGroups(currentBindGroups, vertexCode, fragmentCode) {
        if (!currentBindGroups || typeof currentBindGroups.length !== "number") return null;
        var serializedBindGroups = [];
        for (var groupIndex = 0; groupIndex < currentBindGroups.length; ++groupIndex) {
            var bindGroup = currentBindGroups[groupIndex];
            if (!bindGroup || !bindGroup.entries || typeof bindGroup.entries.length !== "number") continue;

            var layoutEntries = bindGroup.layout && bindGroup.layout.entries ? bindGroup.layout.entries : [];
            var serializedEntries = [];
            for (var i = 0; i < bindGroup.entries.length; ++i) {
                var entry = bindGroup.entries[i];
                if (!entry) continue;
                var resource = entry.resource;
                var binding = entry.binding == null ? 0 : entry.binding;
                var layoutEntry = findLayoutEntry(layoutEntries, binding);
                var visibility = layoutEntry && layoutEntry.visibility != null
                    ? layoutEntry.visibility
                    : inferVisibilityFromShaders(groupIndex, binding, vertexCode, fragmentCode);
                if (resource && resource.buffer && resource.buffer._bytes) {
                    var offset = resource.offset == null ? 0 : resource.offset;
                    var size = resource.size == null ? (resource.buffer.size - offset) : resource.size;
                    if (size < 0) size = 0;
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "buffer",
                        bufferType: layoutEntry && layoutEntry.buffer && layoutEntry.buffer.type ? layoutEntry.buffer.type : "uniform",
                        hasDynamicOffset: !!(layoutEntry && layoutEntry.buffer && layoutEntry.buffer.hasDynamicOffset),
                        minBindingSize: layoutEntry && layoutEntry.buffer && layoutEntry.buffer.minBindingSize != null ? layoutEntry.buffer.minBindingSize : size,
                        size: size,
                        data: cloneBufferBytes({
                            buffer: resource.buffer,
                            offset: offset,
                            size: size
                        })
                    });
                    continue;
                }

                if (resource && resource._objectName === "GPUSampler") {
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "sampler",
                        addressModeU: resource.addressModeU || "clamp-to-edge",
                        addressModeV: resource.addressModeV || "clamp-to-edge",
                        addressModeW: resource.addressModeW || "clamp-to-edge",
                        magFilter: resource.magFilter || "nearest",
                        minFilter: resource.minFilter || "nearest",
                        mipmapFilter: resource.mipmapFilter || "nearest"
                    });
                    continue;
                }

                if (resource && resource._objectName === "GPUTextureView" &&
                    resource._nativeBridge && resource._nativeCanvasId) {
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "textureView",
                        sourceCanvasId: resource._nativeCanvasId,
                        format: resource.format || null,
                        dimension: resource.dimension || "2d",
                        aspect: resource.aspect || "all",
                        baseMipLevel: resource.baseMipLevel == null ? 0 : resource.baseMipLevel,
                        mipLevelCount: resource.mipLevelCount == null ? 1 : resource.mipLevelCount,
                        baseArrayLayer: resource.baseArrayLayer == null ? 0 : resource.baseArrayLayer,
                        arrayLayerCount: resource.arrayLayerCount == null ? 1 : resource.arrayLayerCount
                    });
                    continue;
                }

                return null;
            }

            if (serializedEntries.length > 0) {
                serializedBindGroups.push({
                    index: groupIndex,
                    entries: serializedEntries
                });
            }
        }
        return serializedBindGroups.length > 0 ? serializedBindGroups : null;
    }

    function createAutoBindGroupLayouts(pipelineDescriptor) {
        if (pipelineDescriptor.layout && pipelineDescriptor.layout.bindGroupLayouts) {
            return pipelineDescriptor.layout.bindGroupLayouts;
        }
        if (pipelineDescriptor.layout === "auto") {
            return [ __createMockGPUBindGroupLayout({
                label: (pipelineDescriptor.label || "pipeline") + "-auto-bind-group-layout-0"
            }) ];
        }
        return [];
    }

    function createNativeDrawCommand(attachmentView, currentPipeline, currentBindGroups, vertexCount, instanceCount, firstVertex, firstInstance) {
        if (!attachmentView || !attachmentView._nativeBridge || !attachmentView._nativeCanvasId ||
            !currentPipeline || !currentPipeline._nativeBridge) {
            return null;
        }

        var vertex = currentPipeline.vertex || {};
        var fragment = curre)__JS__"
R"__JS__(ntPipeline.fragment || {};
        var vertexModule = vertex.module || {};
        var fragmentModule = fragment.module || {};
        var command = {
            type: "native-draw-current-texture",
            canvasId: attachmentView._nativeCanvasId,
            vertexCode: vertexModule.code || "",
            vertexEntryPoint: vertex.entryPoint || "main",
            fragmentCode: fragmentModule.code || "",
            fragmentEntryPoint: fragment.entryPoint || "main",
            format: attachmentView.format || (fragment.targets && fragment.targets[0] && fragment.targets[0].format) || __mockPreferredCanvasFormat(),
            topology: currentPipeline.primitive && currentPipeline.primitive.topology ? currentPipeline.primitive.topology : "triangle-list",
            vertexCount: vertexCount == null ? 0 : vertexCount,
            instanceCount: instanceCount == null ? 1 : instanceCount,
            firstVertex: firstVertex == null ? 0 : firstVertex,
            firstInstance: firstInstance == null ? 0 : firstInstance
        };
        var bindGroups = serializeBindGroups(currentBindGroups, vertexModule.code || "", fragmentModule.code || "");
        if (bindGroups) {
            command.bindGroups = bindGroups;
        }
        return command;
    }

    function __createMockGPURenderBundle(init) {
        init = init || {};
        return {
            _objectName: "GPURenderBundle",
            label: init.label || "",
            _commands: init.commands || []
        };
    }

    function __createMockGPURenderBundleEncoder(init) {
        init = init || {};
        var commands = [];
        return {
            _objectName: "GPURenderBundleEncoder",
            label: init.label || "",
            setPipeline: function(pipeline) {
                commands.push({ type: "set-pipeline", pipeline: pipeline || null });
            },
            setBindGroup: function(index, bindGroup) {
                commands.push({ type: "set-bind-group", index: index == null ? 0 : index, bindGroup: bindGroup || null });
            },
            draw: function(vertexCount, instanceCount, firstVertex, firstInstance) {
                commands.push({
                    type: "draw",
                    vertexCount: vertexCount == null ? 0 : vertexCount,
                    instanceCount: instanceCount == null ? 1 : instanceCount,
                    firstVertex: firstVertex == null ? 0 : firstVertex,
                    firstInstance: firstInstance == null ? 0 : firstInstance
                });
            },
            finish: function(descriptor) {
                return __createMockGPURenderBundle({
                    label: descriptor && descriptor.label ? descriptor.label : init.label || "",
                    commands: commands.slice()
                });
            }
        };
    }

    __createMockGPURenderPassEncoder = function(init) {
        init = init || {};
        var descriptor = init.descriptor || {};
        var attachments = descriptor.colorAttachments || [];
        var attachment = attachments.length > 0 ? attachments[0] : null;
        var attachmentView = attachment && attachment.view ? attachment.view : null;
        var nativeCanvasId = attachmentView && attachmentView._nativeCanvasId ? attachmentView._nativeCanvasId : "";
        var passCommands = [];
        var currentPipeline = null;
        var currentBindGroups = [];
        var encoder = originalCreateMockGPURenderPassEncoder(init);
        var originalSetPipeline = encoder.setPipeline;
        var originalSetBindGroup = encoder.setBindGroup;
        var originalDraw = encoder.draw;

        if (attachmentView && attachmentView._nativeBridge && nativeCanvasId && attachment &&
            attachment.loadOp === "clear" && attachment.clearValue) {
            passCommands.push({
                type: "native-clear-current-texture",
                canvasId: nativeCanvasId,
                r: Number(attachment.clearValue.r == null ? 0 : attachment.clearValue.r),
                g: Number(attachment.clearValue.g == null ? 0 : attachment.clearValue.g),
                b: Number(attachment.clearValue.b == null ? 0 : attachment.clearValue.b),
                a: Number(attachment.clearValue.a == null ? 1 : attachment.clearValue.a)
            });
        }

        encoder.setPipeline = function(pipeline) {
            currentPipeline = pipeline || null;
            if (typeof originalSetPipeline === "function") {
                return originalSetPipeline.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.setBindGroup = function(index, bindGroup) {
            currentBindGroups[index == null ? 0 : index] = bindGroup || null;
            if (typeof originalSetBindGroup === "function") {
                return originalSetBindGroup.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.draw = function(vertexCount, instanceCount, firstVertex, firstInstance) {
            if (typeof __installNativeGpuBufferedDrawAugmentation === "function" &&
                __installNativeGpuBufferedDrawAugmentation._installed) {
                return undefined;
            }
            if (typeof originalDraw === "function") {
                originalDraw.apply(encoder, arguments);
            }
            var nativeDraw = createNativeDrawCommand(attachmentView, currentPipeline, currentBindGroups, vertexCount, instanceCount, firstVertex, firstInstance);
            if (nativeDraw) passCommands.push(nativeDraw);
        };

        encoder.executeBundles = function(bundles) {
            if (!bundles || typeof bundles.length !== "number") return;
            for (var i = 0; i < bundles.length; ++i) {
                var bundle = bundles[i];
                var commands = bundle && bundle._commands ? bundle._commands : [];
                for (var j = 0; j < commands.length; ++j) {
                    var command = commands[j];
                    if (!command) continue;
                    if (command.type === "set-pipeline") {
                        encoder.setPipeline(command.pipeline);
                    } else if (command.type === "set-bind-group") {
                        encoder.setBindGroup(command.index, command.bindGroup);
                    } else if (command.type === "draw") {
                        encoder.draw(command.vertexCount, command.instanceCount, command.firstVertex, command.firstInstance);
                    }
                }
            }
        };

        encoder.end = function() {
            if (typeof init.onEnd !== "function") return;
            if (!passCommands.length) {
                init.onEnd(null);
                return;
            }
            for (var i = 0; i < passCommands.length; ++i) {
                init.onEnd(passCommands[i]);
            }
        };
        return encoder;
    };

    __createMockGPURenderPipeline = function(init) {
        init = init || {};
        var pipeline = originalCreateMockGPURenderPipeline(init);
        pipeline._nativeBridge = !!init.nativeBridge;
        pipeline.vertex = init.vertex || null;
        pipeline.fragment = init.fragment || null;
        pipeline.primitive = init.primitive || null;
        return pipeline;
    };

    __createMockGPUQueue = function(init) {
        var queue = originalCreateMockGPUQueue(init || {});
        var originalSubmit = queue.submit;
        queue.submit = function(commandBuffers) {
            if (typeof originalSubmit === "function") {
                originalSubmit.apply(queue, arguments);
            }
            if (!queue._nativeBridge || typeof __gpuQueueDrawImpl !== "function" || !commandBuffers) {
                return;
            }
            var bufferedInstalled = typeof __installNativeGpuBufferedDrawAugmentation === "function" &&
                __installNativeGpuBufferedDrawAugmentation._installed;
            for (var i = 0; i < commandBuffers.length; ++i) {
                var commandBuffer = commandBuffers[i];
                var commands = commandBuffer && commandBuffer._commands ? commandBuffer._commands : [];
                for (var j = 0; j < commands.length; ++j) {
                    var command = commands[j];
                    if (command && command.type === "native-draw-current-texture") {
                        if (bufferedInstalled) {
                            continue;
                        }
                        var bindGroupsPayload = command.bindGroups ? JSON.stringify(command.bindGroups) : "";
                        var drawOk = __gpuQueueDrawImpl(
                            command.canvasId,
                            command.vertexCode,
                            command.vertexEntryPoint,
                            command.fragmentCode,
                            command.fragmentEntryPoint,
                            command.format,
                            command.topology,
                            command.vertexCount,
                            command.instanceCount,
                            command.firstVertex,
                            command.firstInstance,
                            bindGroupsPayload
                        );
                        if (drawOk === false) {
                            throw new Error("Native GPU draw replay failed");
                        }
                    }
                }
            }
        };
        return queue;
    };

    __createMockGPUDevice = function(adapter, descriptor, init) {
        var device = originalCreateMockGPUDevice(adapter, descriptor, init || {});
        device.createRenderPipeline = function(pipelineDescriptor) {
            pipelineDescriptor = pipelineDescriptor || {};
            return __createMockGPURenderPipeline({
                label: pipelineDescriptor.label || "",
                nativeBridge: !!device._nativeBridge,
                vertex: pipelineDescriptor.vertex || null,
                fragment: pipelineDescriptor.fragment || null,
                primitive: pipelineDescriptor.primitive || null,
                bindGroupLayouts: createAutoBindGroupLayouts(pipelineDescriptor)
            });
        };
        device.createRenderBundleEncoder = function(bundleDescriptor) {
            return __createMockGPURenderBundleEncoder(bundleDescriptor || {});
        };
        return device;
    };

    __installNativeGpuCommandAugmentation._installed = true;
}

function __createGPUCanvasContext(canvasEl) {
    __ensurePulpGpuHelpers();
    var context = {
        _objectName: "GPUCanvasContext",
        canvas: canvasEl,
        _configured: false,
        _nativeBridge: false,
        device: null,
        format: "bgra8unorm",
        usage: 0x10,
        alphaMode: "opaque"
    };
    context.configure = function(descriptor) {
        descriptor = descriptor || {};
        context._configured = true;
        context.device = descriptor.device || null;
        context.format = descriptor.format || (typeof __mockPreferredCanvasFormat === "function"
            ? __mockPreferredCanvasFormat() : "bgra8unorm");
        context.usage = descriptor.usage || (typeof GPUTextureUsage !== "undefined"
            ? GPUTextureUsage.RENDER_ATTACHMENT : 0x10);
        context.alphaMode = descriptor.alphaMode || "opaque";
        context._nativeBridge = false;

        if (context.device && context.device._nativeBridge && typeof __gpuCanvasConfigureImpl === "function") {
            var nativeState = __gpuCanvasConfigureImpl(
                context.canvas && context.canvas._id ? context.canvas._id : "",
                context.canvas && context.canvas.width ? context.canvas.width : 1,
                context.canvas && context.canvas.height ? context.canvas.height : 1,
                context.format,
                context.usage,
                context.alphaMode
            ) || {};
            context._nativeBridge = !!nativeState.nativeBridge;
        )__JS__"
R"__JS__(    context._configured = !!nativeState.configured;
        }
    };
    context.getCurrentTexture = function() {
        if (context._nativeBridge && typeof __gpuCanvasDescribeCurrentTextureImpl === "function") {
            var nativeTexture = __gpuCanvasDescribeCurrentTextureImpl(context.canvas && context.canvas._id ? context.canvas._id : "") || {};
            var bridgedTexture = __createMockGPUTexture({
                size: {
                    width: nativeTexture.width || (context.canvas && context.canvas.width ? context.canvas.width : 1),
                    height: nativeTexture.height || (context.canvas && context.canvas.height ? context.canvas.height : 1)
                },
                format: nativeTexture.format || context.format,
                usage: nativeTexture.usage || context.usage,
                label: nativeTexture.label || ((context.canvas && context.canvas.id ? context.canvas.id : "pulp-canvas") + "-current-texture"),
                nativeBridge: !!nativeTexture.nativeBridge,
                nativeCanvasId: context.canvas && context.canvas._id ? context.canvas._id : ""
            });
            bridgedTexture._nativeBridge = !!nativeTexture.nativeBridge;
            return bridgedTexture;
        }
        var mockTexture = __createMockGPUTexture({
            size: {
                width: context.canvas && context.canvas.width ? context.canvas.width : 1,
                height: context.canvas && context.canvas.height ? context.canvas.height : 1
            },
            format: context.format,
            usage: context.usage,
            label: (context.canvas && context.canvas.id ? context.canvas.id : "pulp-canvas") + "-current-texture"
        });
        mockTexture._nativeBridge = false;
        return mockTexture;
    };
    context.present = function() {
        if (context._nativeBridge && typeof __gpuCanvasPresentImpl === "function") {
            return __gpuCanvasPresentImpl(context.canvas && context.canvas._id ? context.canvas._id : "");
        }
        return undefined;
    };
    return context;
}

function __createMockGPUCanvasContext(canvasEl) {
    return __createGPUCanvasContext(canvasEl);
}

function _coerceCanvasDimension(value, fallback) {
    var n = parseInt(value, 10);
    if (!(n > 0)) return fallback;
    return n;
}

Object.defineProperty(Element.prototype, "width", {
    get: function() {
        if (this.tagName.toLowerCase() !== "canvas") return 0;
        return this._canvasWidth || 300;
    },
    set: function(v) {
        if (this.tagName.toLowerCase() !== "canvas") return;
        var width = _coerceCanvasDimension(v, 300);
        this._canvasWidth = width;
        this.style.width = width + "px";
    }
});

Object.defineProperty(Element.prototype, "height", {
    get: function() {
        if (this.tagName.toLowerCase() !== "canvas") return 0;
        return this._canvasHeight || 150;
    },
    set: function(v) {
        if (this.tagName.toLowerCase() !== "canvas") return;
        var height = _coerceCanvasDimension(v, 150);
        this._canvasHeight = height;
        this.style.height = height + "px";
    }
});

Element.prototype.getContext = function(kind) {
    if (this.tagName.toLowerCase() !== "canvas") return null;
    if (kind === "2d") {
        if (!this._canvasContext2d) this._canvasContext2d = new CanvasRenderingContext2D(this);
        return this._canvasContext2d;
    }
    if (kind === "webgpu") {
        if (!this._canvasContextWebgpu && typeof __createGPUCanvasContext === "function") {
            this._canvasContextWebgpu = __createGPUCanvasContext(this);
        } else if (!this._canvasContextWebgpu && typeof __createMockGPUCanvasContext === "function") {
            this._canvasContextWebgpu = __createMockGPUCanvasContext(this);
        }
        return this._canvasContextWebgpu || null;
    }
    return null;
};
)__JS__"
;

static const char* web_compat_style_decl =
R"__JS__(// ═══════════════════════════════════════════════════════════════════════════════
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
     )__JS__"
R"__JS__(   case "filter":
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

        // ── Tier 2: Per-side borders ────────────────────────────────────

        case "borderTop": case "borderRight": case "borderBottom": case "borderLeft": {
            var side = key.replace("border", "").toLowerCase();
            var bsp = resolved.match(/([\d.]+)px\s+\w+\s+(.+)/);
            if (bsp) {
                var bsc = parseCSSColor(bsp[2].trim());
                if (typeof setBorderSide === "function")
                    setBorderSide(id, side, parseFloat(bsp[1]), bsc || bsp[2].trim());
            }
            break;
        }
        case "borderTopWidth": case "borderRightWidth": case "borderBottomWidth": case "borderLeftWidth": {
            var side2 = key.replace("border", "").replace("Width", "").toLowerCase();
            var bw2 = parseCSSLength(resolved);
            if (bw2 && typeof setBorderSide === "function")
                setBorderSide(id, side2, bw2.value, "");
            break;
        }
        case "borderTopColor": case "borderRightColor": case "borderBottomColor": case "borderLeftColor": {
            var side3 = key.replace("border", "").replace("Color", "").toLowerCase();
            var bc3 = parseCSSColor(resolved);
            if (bc3 && typeof setBorderSide === "function")
                setBorderSide(id, side3, 0, bc3);
            break;
        }

        // Per-corner border-radius
        case "borderTopLeftRadius": case "borderTopRightRadius":
        case "borderBottomLeftRadius": case "borderBottomRightRadius": {
            var corner = key.replace("border", "").replace("Radius", "");
            var cr = parseCSSLength(resolved);
            if (cr && typeof setCornerRadius === "function")
                setCornerRadius(id, corner, cr.value);
            break;
        }

        // ── Tier 3: Layout keywords ─────────────────────────────────────

        // box-sizing
        case "boxSizing":
            if (typeof setBoxSizing === "function") setBoxSizing(id, resolved);
            break;

        // flex-flow shorthand
        case "flexFlow": {
            var ffp = resolved.split(/\s+/);
            for (var ffi = 0; ffi < ffp.length; ffi++) {
                if (ffp[ffi] === "row" || ffp[ffi] === "column")
                    setFlex(id, "direction", ffp[ffi] === "row" ? "row" : "col");
                else if (ffp[ffi] === "wrap" || ffp[ffi] === "nowrap")
                    setFlex(id, "flex_wrap", ffp[ffi] === "wrap" ? 1 : 0);
            }
            break;
        }

        // place-items shorthand (align-items + justify-items)
        case "placeItems": {
            var pip = resolved.split(/\s+/);
            setFlex(id, "align_items", _cssToFlex(pip[0]));
            if (pip[1]) setFlex(id, "justify_content", _cssToFlex(pip[1]));
            break;
        }

        // place-content shorthand
        case "placeContent": {
            var pcp = resolved.split(/\s+/);
            setFlex(id, "align_content", _cssToFlex(pcp[0]));
            if (pcp[1]) setFlex(id, "justify_content", _cssToFlex(pcp[1]));
            break;
        }

        // ── Tier 4: Animation properties ────────────────────────────────

        case "animationName":
            if (typeof setAnimation === "function") setAnimation(id, "name", resolved);
            break;
        case "animationDuration": {
            var ad = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) ad /= 1000;
            if (typeof setAnimation === "function") setAnimation(id, "duration", ad);
            break;
        }
        case "animationTimingFunction":
            if (typeof setAnimation === "function") setAnimation(id, "easing", resolved);
            break;
        case "animationDelay": {
            var adl = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) adl /= 1000;
            if (typeof setAnimation === "function") setAnimation(id, "delay", adl);
            break;
        }
        case "animationIterationCount":
            if (typeof setAnimation === "function")
                setAnimation(id, "iterations", resolved === "infinite" ? -1 : parseFloat(resolved) || 1);
            break;
        case "animationDirection":
            if (typeof setAnimation === "function") setAnimation(id, "direction", resolved);
            break;
        case "animationFillMode":
            if (typeof setAnimation === "function") setAnimation(id, "fill", resolved);
            break;
        case "animation": {
            // Shorthand: "name duration easing delay iterations direction fill"
            var atr = parseTransition(resolved); // reuse transition parser for timing
            if (typeof setAnimation === "function") {
                setAnimation(id, "name", atr.property);
                setAnimation(id, "duration", atr.duration);
                setAnimation(id, "easing", atr.easing);
                setAnimation(id, "delay", atr.delay);
            }
            break;
        }

        // ── Tier 6: Additional filter functions ─────────────────────────

        // filter already handled above — extend for multi-function
        // (the existing setFilter bridge parses "blur(4px)" — additional functions pass through)

        // ── Tier 7: CSS logical properties ──────────────────────────────

        case "marginInline": {
            var mi = expandShorthand(resolved);
            setFlex(id, "margin_left", mi[0]); setFlex(id, "margin_right", mi[1]);
            break;
        }
        case "marginInlineStart":
        case "marginLeft": // already handled above, fall through for logical
            break;
        case "marginBlock": {
            var mb2 = expandShorthand(resolved);
            setFlex(id, "margin_top", mb2[0]); setFlex(id, "margin_bottom", mb2[1]);
            break;
        }
        case "paddingInline": {
            var pi2 = expandShorthand(resolved);
            setFlex(id, "padding_left", pi2[0]); setFlex(id, "padding_right", pi2[1]);
            break;
        }
        case "paddingBlock": {
            var pb2 = expandShorthand(resolved);
            setFlex(id, "padding_top", pb2[0]); setFlex(id, "padding_bottom", pb2[1]);
            break;
        }
        case "inset": {
            var ins = expandShorthand(resolved);
            var tv2 = parseCSSLength(String(ins[0])); if (tv2) setTop(id, tv2.value);
            var rv2 = parseCSSLength(String(ins[1])); if (rv2) setRight(id, rv2.value);
            var bv2 = parseCSSLength(String(ins[2])); if (bv2) setBottom(id, bv2.value);
            var lv2 = parseCSSLength(String(ins[3])); if (lv2) setLeft(id, lv2.value);
            break;
        }

        // line-clamp (-webkit-line-clamp)
        case "webkitLineClamp":
        case "lineClamp":
            if (typeof setLineClamp === "function") setLineClamp(id, parseInt(resolved) || 0);
            break;

        // background-repeat
        case "backgroundRepeat":
            if (typeof setBackgroundRepeat === "function") setBa)__JS__"
R"__JS__(ckgroundRepeat(id, resolved);
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
    "flexFlow", "justifyContent", "alignItems", "alignSelf", "alignContent", "order",
    "placeItems", "placeContent",
    "gap", "rowGap", "columnGap",
    "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
    "aspectRatio", "boxSizing",
    "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
    "marginInline", "marginBlock",
    "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "paddingInline", "paddingBlock",
    "backgroundColor", "color",
    "fontSize", "fontWeight", "fontStyle", "fontFamily", "letterSpacing", "lineHeight",
    "textAlign", "textTransform", "textDecoration", "textOverflow", "textShadow",
    "whiteSpace", "wordBreak", "overflowWrap", "wordWrap",
    "border", "borderColor", "borderWidth", "borderRadius",
    "borderTop", "borderRight", "borderBottom", "borderLeft",
    "borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth",
    "borderTopColor", "borderRightColor", "borderBottomColor", "borderLeftColor",
    "borderTopLeftRadius", "borderTopRightRadius", "borderBottomLeftRadius", "borderBottomRightRadius",
    "outline", "outlineWidth", "outlineColor",
    "opacity", "overflow", "cursor", "visibility",
    "userSelect", "pointerEvents",
    "transform", "transformOrigin",
    "transition", "transitionDuration",
    "animation", "animationName", "animationDuration", "animationTimingFunction",
    "animationDelay", "animationIterationCount", "animationDirection", "animationFillMode",
    "position", "top", "right", "bottom", "left", "zIndex", "inset",
    "boxShadow", "filter", "background", "backgroundImage",
    "backgroundSize", "backgroundPosition", "backgroundRepeat",
    "gridTemplateColumns", "gridTemplateRows", "gridColumn", "gridRow",
    "lineClamp", "webkitLineClamp"
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
)__JS__"
;

static const char* web_compat_document =
R"__JS__(// ═══════════════════════════════════════════════════════════════════════════════
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
        var el = new Element("span");
        el._textContent = text;
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

__installGlobalIfMissing()__JS__"
R"__JS__("GPUShaderStage", {
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
    return {
        _objectName: "GPUComputePassEncoder",
        label: init.label || "",
        setPipeline: function() {},
        setBindGroup: function() {},
        dispatchWorkgroups: function() {},
        dispatchWorkgroupsIndirect: function() {},
        end: function() {}
    };
}

function __createMockGPUCommandEncoder(init) {
    init = init || {};
    return {
        _objectName: "GPUCommandEncoder",
        label: init.label || "",
        beginRenderPass: function(descriptor) {
            return __createMockGPURenderPassEncoder({
                label: descriptor && descriptor.label ? descriptor.label : "",
                descriptor: descriptor || {}
            });
        },
        beginComputePass: function(descriptor) {
            return __createMockGPUComputePassEncoder({ label: descriptor && descriptor.label ? descriptor.label : "" });
        },
        copyBufferToBuffer: function() {},
        copyTextureToBuffer: function() {},
        copyBufferToTexture: function() {},
        finish: function(descriptor) {
            return __createMockGPUCommandBuffer({ label: descriptor && descriptor.label ? descriptor.label : "" });
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
  )__JS__"
R"__JS__(  }
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
    this._map[String(name || "").toLowerCase()] = String()__JS__"
R"__JS__(value == null ? "" : value);
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
)__JS__"
;

static const char* web_compat_dom_ops =
R"__JS__(// DOM manipulation methods — small file for QuickJS compilation limit

Element.prototype.appendChild = function(child) {
    if (!(child instanceof Element)) return child;
    if (child._parentElement) child._parentElement.removeChild(child);
    child._parentElement = this;
    this._children.push(child);
    this._ensureNative();
    __domAppend(this._id, child._id, child.tagName.toLowerCase());
    child._nativeCreated = true;
    if (child._textContent) setText(child._id, child._textContent);
    child.style._flushAll();
    child._reapplyStylesheets();
    return child;
};

Element.prototype.removeChild = function(child) {
    var idx = this._children.indexOf(child);
    if (idx < 0) return child;
    this._children.splice(idx, 1);
    child._parentElement = null;
    if (child._nativeCreated) __domRemove(child._id);
    child._nativeCreated = false;
    return child;
};

Element.prototype.remove = function() {
    if (this._parentElement) this._parentElement.removeChild(this);
};

Element.prototype.insertBefore = function(newChild, refChild) {
    if (!refChild) return this.appendChild(newChild);
    var idx = this._children.indexOf(refChild);
    if (idx < 0) return this.appendChild(newChild);
    if (newChild._parentElement) newChild._parentElement.removeChild(newChild);
    newChild._parentElement = this;
    this._children.splice(idx, 0, newChild);
    this._ensureNative();
    __domAppend(this._id, newChild._id, newChild.tagName.toLowerCase());
    newChild._nativeCreated = true;
    if (newChild._textContent) setText(newChild._id, newChild._textContent);
    newChild.style._flushAll();
    newChild._reapplyStylesheets();
    return newChild;
};

Element.prototype.replaceChild = function(newChild, oldChild) {
    var idx = this._children.indexOf(oldChild);
    if (idx < 0) return oldChild;
    this.removeChild(oldChild);
    this.appendChild(newChild);
    return oldChild;
};
)__JS__"
;

static const char* web_compat_gpu_buffered =
R"__JS__(// ═══════════════════════════════════════════════════════════════════════════════
// Native GPU buffered draw augmentation
// ═══════════════════════════════════════════════════════════════════════════════

function __installNativeGpuBufferedDrawAugmentation() {
    if (typeof __createMockGPURenderPassEncoder !== "function" ||
        typeof __createMockGPUQueue !== "function" ||
        typeof __createMockGPUDevice !== "function") {
        return;
    }
    if (__installNativeGpuBufferedDrawAugmentation._installed) return;

    function cloneBufferBytes(binding) {
        if (!binding || !binding.buffer || !binding.buffer._bytes) return [];
        var source = binding.buffer._bytes;
        var begin = binding.offset == null ? 0 : binding.offset;
        var end = binding.size == null ? source.length : begin + binding.size;
        if (begin < 0) begin = 0;
        if (end < begin) end = begin;
        return Array.from(source.slice(begin, end));
    }

    function noteBufferedSkip(reason, details) {
        try {
            if (typeof globalThis !== "undefined") {
                if (!globalThis.__phase13BufferedSkips) {
                    globalThis.__phase13BufferedSkips = [];
                }
                globalThis.__phase13BufferedSkips.push(JSON.stringify({
                    reason: reason,
                    details: details || {}
                }));
            }
        } catch (_) {}
    }

    function findLayoutEntry(layoutEntries, binding) {
        if (!layoutEntries || typeof layoutEntries.length !== "number") return null;
        for (var i = 0; i < layoutEntries.length; ++i) {
            var entry = layoutEntries[i];
            if (entry && entry.binding === binding) return entry;
        }
        return null;
    }

    function shaderUsesBinding(code, groupIndex, binding) {
        if (!code) return false;
        var bindingThenGroup = new RegExp("@binding\\s*\\(\\s*" + binding + "\\s*\\)\\s*@group\\s*\\(\\s*" + groupIndex + "\\s*\\)");
        var groupThenBinding = new RegExp("@group\\s*\\(\\s*" + groupIndex + "\\s*\\)\\s*@binding\\s*\\(\\s*" + binding + "\\s*\\)");
        return bindingThenGroup.test(code) || groupThenBinding.test(code);
    }

    function inferVisibilityFromShaders(groupIndex, binding, vertexCode, fragmentCode) {
        var visibility = 0;
        if (shaderUsesBinding(vertexCode, groupIndex, binding)) {
            visibility |= (typeof GPUShaderStage !== "undefined") ? GPUShaderStage.VERTEX : 0x1;
        }
        if (shaderUsesBinding(fragmentCode, groupIndex, binding)) {
            visibility |= (typeof GPUShaderStage !== "undefined") ? GPUShaderStage.FRAGMENT : 0x2;
        }
        return visibility || ((typeof GPUShaderStage !== "undefined") ? (GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT) : 0x3);
    }

    function serializeBindGroups(currentBindGroups, vertexCode, fragmentCode) {
        if (!currentBindGroups || typeof currentBindGroups.length !== "number") return null;
        var serializedBindGroups = [];
        for (var groupIndex = 0; groupIndex < currentBindGroups.length; ++groupIndex) {
            var bindGroup = currentBindGroups[groupIndex];
            if (!bindGroup || !bindGroup.entries || typeof bindGroup.entries.length !== "number") continue;

            var layoutEntries = bindGroup.layout && bindGroup.layout.entries ? bindGroup.layout.entries : [];
            var serializedEntries = [];
            for (var i = 0; i < bindGroup.entries.length; ++i) {
                var entry = bindGroup.entries[i];
                if (!entry) continue;
                var resource = entry.resource;
                var binding = entry.binding == null ? 0 : entry.binding;
                var layoutEntry = findLayoutEntry(layoutEntries, binding);
                var visibility = layoutEntry && layoutEntry.visibility != null
                    ? layoutEntry.visibility
                    : inferVisibilityFromShaders(groupIndex, binding, vertexCode, fragmentCode);
                if (resource && resource.buffer && resource.buffer._bytes) {
                    var offset = resource.offset == null ? 0 : resource.offset;
                    var size = resource.size == null ? (resource.buffer.size - offset) : resource.size;
                    if (size < 0) size = 0;
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "buffer",
                        bufferType: layoutEntry && layoutEntry.buffer && layoutEntry.buffer.type ? layoutEntry.buffer.type : "uniform",
                        hasDynamicOffset: !!(layoutEntry && layoutEntry.buffer && layoutEntry.buffer.hasDynamicOffset),
                        minBindingSize: layoutEntry && layoutEntry.buffer && layoutEntry.buffer.minBindingSize != null ? layoutEntry.buffer.minBindingSize : size,
                        size: size,
                        data: cloneBufferBytes({
                            buffer: resource.buffer,
                            offset: offset,
                            size: size
                        })
                    });
                    continue;
                }

                if (resource && resource._objectName === "GPUSampler") {
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "sampler",
                        addressModeU: resource.addressModeU || "clamp-to-edge",
                        addressModeV: resource.addressModeV || "clamp-to-edge",
                        addressModeW: resource.addressModeW || "clamp-to-edge",
                        magFilter: resource.magFilter || "nearest",
                        minFilter: resource.minFilter || "nearest",
                        mipmapFilter: resource.mipmapFilter || "nearest"
                    });
                    continue;
                }

                if (resource && resource._objectName === "GPUTextureView" &&
                    resource._nativeBridge && resource._nativeTextureId) {
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "textureView",
                        sourceTextureId: resource._nativeTextureId,
                        format: resource.format || null,
                        dimension: resource.dimension || "2d",
                        aspect: resource.aspect || "all",
                        baseMipLevel: resource.baseMipLevel == null ? 0 : resource.baseMipLevel,
                        mipLevelCount: resource.mipLevelCount == null ? 1 : resource.mipLevelCount,
                        baseArrayLayer: resource.baseArrayLayer == null ? 0 : resource.baseArrayLayer,
                        arrayLayerCount: resource.arrayLayerCount == null ? 1 : resource.arrayLayerCount
                    });
                    continue;
                }

                if (resource && resource._objectName === "GPUTextureView" &&
                    resource._nativeBridge && resource._nativeCanvasId) {
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "textureView",
                        sourceCanvasId: resource._nativeCanvasId,
                        format: resource.format || null,
                        dimension: resource.dimension || "2d",
                        aspect: resource.aspect || "all",
                        baseMipLevel: resource.baseMipLevel == null ? 0 : resource.baseMipLevel,
                        mipLevelCount: resource.mipLevelCount == null ? 1 : resource.mipLevelCount,
                        baseArrayLayer: resource.baseArrayLayer == null ? 0 : resource.baseArrayLayer,
                        arrayLayerCount: resource.arrayLayerCount == null ? 1 : resource.arrayLayerCount
                    });
                    continue;
                }

                if (resource && resource._objectName === "GPUTextureView" &&
                    resource.texture && resource.texture._bytes) {
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "textureView",
                        format: resource.format || (resource.texture && resource.texture.format) || null,
                        dimension: resource.dimension || "2d",
                        aspect: resource.aspect || "all",
                        baseMipLevel: resource.baseMipLevel == null ? 0 : resource.baseMipLevel,
                        mipLevelCount: resource.mipLevelCount == null ? 1 : resource.mipLevelCount,
                        baseArrayLayer: resource.baseArrayLayer == null ? 0 : resource.baseArrayLayer,
                        arrayLayerCount: resource.arrayLayerCount == null ? 1 : resource.arrayLayerCount,
                        width: resource.texture.width || 1,
                        height: resource.texture.height || 1,
                        depthOrArrayLayers: resource.texture.depthOrArrayLayers || 1,
                        usage: resource.texture.usage || 0,
                        sampleCount: resource.texture.sampleCount || 1,
                        textureMipLevelCount: resource.texture.mipLevelCount || 1,
                        bytesPerRow: resource.texture._bytesPerRow || 0,
                        rowsPerImage: resource.texture._rowsPerImage || resource.texture.height || 1,
                        data: Array.from(resource.texture._bytes)
                    });
                    continue;
                }

                return null;
            }

            if (serializedEntries.length > 0) {
                serializedBindGroups.push({
                    index: groupIndex,
                    entries: serializedEntries
                });
            }
        }
        return serializedBindGroups.length > 0 ? serializedBindGroups : null;
    }

    function createBufferedDrawPayload(attachment, attachmentView, currentPipeline, currentBindGroups, currentVertexBuffers, currentIndexBuffer, drawDescriptor) {
        if (!attachmentView || !attachmentView._nativeBridge ||
            !currentPipeline || !currentPipeline._nativeBridge || !drawDescriptor) {
            noteBufferedSkip("missing-native-bridge", {
                attachmentNativeBridge: !!(attachmentView && attachmentView._nativeBridge),
                attachmentCanvasId: attachmentView && attachmentView._nativeCanvasId ? attachmentView._nativeCanvasId : "",
                attachmentTextureId: attachmentView && attachmentView._nativeTextureId ? attachmentView._nativeTextureId : "",
                pipelineNativeBridge: !!(currentPipeline && currentPipeline._nativeBridge)
            });
            return null;
        }

        var vertex = currentPipeline.vertex || {};
        var fragment = currentPipeline.fragment || {};
        var vertexModule = vertex.module || {};
        var fragmentModule = fragment.module || {};
        var vertexLayouts = vertex.buffers || [];
        var serializedVertexBuffers = [];
        var hasVertexBuffer = false;

        for (var slot = 0; slot < currentVertexBuffers.length; ++slot) {
            var binding = currentVertexBuffers[slot];
            if (!binding) continue;
            hasVertexBuffer = true;
            var layout = vertexLayouts[slot] || {};
            var attributes = layout.attri)__JS__"
R"__JS__(butes || [];
            var serializedAttributes = [];
            for (var i = 0; i < attributes.length; ++i) {
                var attribute = attributes[i] || {};
                serializedAttributes.push({
                    shaderLocation: attribute.shaderLocation == null ? 0 : attribute.shaderLocation,
                    format: attribute.format || "float32x2",
                    offset: attribute.offset == null ? 0 : attribute.offset
                });
            }
            serializedVertexBuffers.push({
                slot: slot,
                arrayStride: layout.arrayStride == null ? 0 : layout.arrayStride,
                stepMode: layout.stepMode || "vertex",
                attributes: serializedAttributes,
                data: cloneBufferBytes(binding)
            });
        }

        if (!hasVertexBuffer) {
            noteBufferedSkip("missing-vertex-buffer", {
                attachmentCanvasId: attachmentView && attachmentView._nativeCanvasId ? attachmentView._nativeCanvasId : "",
                attachmentTextureId: attachmentView && attachmentView._nativeTextureId ? attachmentView._nativeTextureId : ""
            });
            return null;
        }

        var payload = {
            vertexCode: vertexModule.code || "",
            vertexEntryPoint: vertex.entryPoint || "main",
            fragmentCode: fragmentModule.code || "",
            fragmentEntryPoint: fragment.entryPoint || "main",
            format: attachmentView.format || (fragment.targets && fragment.targets[0] && fragment.targets[0].format) || __mockPreferredCanvasFormat(),
            topology: currentPipeline.primitive && currentPipeline.primitive.topology ? currentPipeline.primitive.topology : "triangle-list",
            vertexBuffers: serializedVertexBuffers,
            drawType: drawDescriptor.drawType || "draw"
        };
        if (attachment) {
            payload.loadOp = attachment.loadOp || "load";
            payload.storeOp = attachment.storeOp || "store";
            if (attachment.clearValue) {
                payload.clearValue = {
                    r: Number(attachment.clearValue.r == null ? 0 : attachment.clearValue.r),
                    g: Number(attachment.clearValue.g == null ? 0 : attachment.clearValue.g),
                    b: Number(attachment.clearValue.b == null ? 0 : attachment.clearValue.b),
                    a: Number(attachment.clearValue.a == null ? 1 : attachment.clearValue.a)
                };
            }
        }
        // Serialize depth/stencil attachment if present
        if (depthStencil) {
            payload.depthStencil = {
                depthLoadOp: depthStencil.depthLoadOp || "clear",
                depthStoreOp: depthStencil.depthStoreOp || "store",
                depthClearValue: depthStencil.depthClearValue == null ? 1.0 : depthStencil.depthClearValue,
                format: depthStencil.view && depthStencil.view.texture
                    ? depthStencil.view.texture.format || "depth24plus"
                    : "depth24plus"
            };
        }
        // Serialize depth/stencil state from pipeline
        if (currentPipeline && currentPipeline.depthStencil) {
            payload.pipelineDepthStencil = {
                format: currentPipeline.depthStencil.format || "depth24plus",
                depthWriteEnabled: currentPipeline.depthStencil.depthWriteEnabled !== false,
                depthCompare: currentPipeline.depthStencil.depthCompare || "less"
            };
        }

        if (attachmentView._nativeCanvasId) {
            payload.canvasId = attachmentView._nativeCanvasId;
        } else if (attachmentView._nativeTextureId) {
            payload.targetTextureId = attachmentView._nativeTextureId;
        } else {
            noteBufferedSkip("missing-native-target", {
                attachmentNativeBridge: !!attachmentView._nativeBridge
            });
            return null;
        }

        var bindGroups = serializeBindGroups(currentBindGroups, vertexModule.code || "", fragmentModule.code || "");
        if (bindGroups) {
            payload.bindGroups = bindGroups;
        }

        if (currentIndexBuffer && currentIndexBuffer.buffer && currentIndexBuffer.buffer._bytes) {
            payload.indexBuffer = {
                format: currentIndexBuffer.format || "uint32",
                data: cloneBufferBytes(currentIndexBuffer)
            };
        }

        if (drawDescriptor.drawType === "draw-indexed") {
            payload.indexCount = drawDescriptor.indexCount == null ? 0 : drawDescriptor.indexCount;
            payload.instanceCount = drawDescriptor.instanceCount == null ? 1 : drawDescriptor.instanceCount;
            payload.firstIndex = drawDescriptor.firstIndex == null ? 0 : drawDescriptor.firstIndex;
            payload.baseVertex = drawDescriptor.baseVertex == null ? 0 : drawDescriptor.baseVertex;
            payload.firstInstance = drawDescriptor.firstInstance == null ? 0 : drawDescriptor.firstInstance;
        } else {
            payload.vertexCount = drawDescriptor.vertexCount == null ? 0 : drawDescriptor.vertexCount;
            payload.instanceCount = drawDescriptor.instanceCount == null ? 1 : drawDescriptor.instanceCount;
            payload.firstVertex = drawDescriptor.firstVertex == null ? 0 : drawDescriptor.firstVertex;
            payload.firstInstance = drawDescriptor.firstInstance == null ? 0 : drawDescriptor.firstInstance;
        }

        return payload;
    }

    function createPresentTexturePayload(attachmentView, currentPipeline, currentBindGroups) {
        if (!attachmentView || !attachmentView._nativeBridge || !attachmentView._nativeCanvasId) {
            return null;
        }

        if (!currentBindGroups || typeof currentBindGroups.length !== "number") {
            return null;
        }

        for (var groupIndex = 0; groupIndex < currentBindGroups.length; ++groupIndex) {
            var bindGroup = currentBindGroups[groupIndex];
            if (!bindGroup || !bindGroup.entries || typeof bindGroup.entries.length !== "number") continue;
            for (var i = 0; i < bindGroup.entries.length; ++i) {
                var entry = bindGroup.entries[i];
                var resource = entry && entry.resource ? entry.resource : null;
                if (resource && resource._objectName === "GPUTextureView" &&
                    resource._nativeBridge && resource._nativeTextureId) {
                    return {
                        canvasId: attachmentView._nativeCanvasId,
                        sourceTextureId: resource._nativeTextureId
                    };
                }
            }
        }

        return null;
    }

    var originalCreateMockGPURenderPassEncoder = __createMockGPURenderPassEncoder;
    var originalCreateMockGPUQueue = __createMockGPUQueue;
    var originalCreateMockGPUDevice = __createMockGPUDevice;

    __createMockGPURenderPassEncoder = function(init) {
        var encoder = originalCreateMockGPURenderPassEncoder(init || {});
        var currentPipeline = null;
        var currentBindGroups = [];
        var currentVertexBuffers = [];
        var currentIndexBuffer = null;
        var emittedBufferedDraw = false;
        var descriptor = init && init.descriptor ? init.descriptor : {};
        var attachments = descriptor.colorAttachments || [];
        var attachment = attachments.length > 0 ? attachments[0] : null;
        var attachmentView = attachment && attachment.view ? attachment.view : null;
        var depthStencil = descriptor.depthStencilAttachment || null;
        var originalSetPipeline = encoder.setPipeline;
        var originalSetBindGroup = encoder.setBindGroup;
        var originalSetVertexBuffer = encoder.setVertexBuffer;
        var originalSetIndexBuffer = encoder.setIndexBuffer;
        var originalDraw = encoder.draw;
        var originalDrawIndexed = encoder.drawIndexed;
        var originalExecuteBundles = encoder.executeBundles;

        encoder.setPipeline = function(pipeline) {
            currentPipeline = pipeline || null;
            if (typeof originalSetPipeline === "function") {
                return originalSetPipeline.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.setBindGroup = function(index, bindGroup) {
            currentBindGroups[index == null ? 0 : index] = bindGroup || null;
            if (typeof originalSetBindGroup === "function") {
                return originalSetBindGroup.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.setVertexBuffer = function(slot, buffer, offset, size) {
            currentVertexBuffers[slot == null ? 0 : slot] = {
                buffer: buffer || null,
                offset: offset == null ? 0 : offset,
                size: size
            };
            if (typeof originalSetVertexBuffer === "function") {
                return originalSetVertexBuffer.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.setIndexBuffer = function(buffer, format, offset, size) {
            currentIndexBuffer = {
                buffer: buffer || null,
                format: format || "uint32",
                offset: offset == null ? 0 : offset,
                size: size
            };
            if (typeof originalSetIndexBuffer === "function") {
                return originalSetIndexBuffer.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.draw = function(vertexCount, instanceCount, firstVertex, firstInstance) {
            var bufferedPayload = createBufferedDrawPayload(attachment, attachmentView, currentPipeline, currentBindGroups, currentVertexBuffers, currentIndexBuffer, {
                drawType: "draw",
                vertexCount: vertexCount,
                instanceCount: instanceCount,
                firstVertex: firstVertex,
                firstInstance: firstInstance
            });
            if (bufferedPayload && typeof init.onEnd === "function") {
                emittedBufferedDraw = true;
                init.onEnd({
                    type: "native-draw-current-texture-buffered",
                    payload: bufferedPayload
                });
                return;
            }
            if (typeof originalDraw === "function") {
                return originalDraw.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.drawIndexed = function(indexCount, instanceCount, firstIndex, baseVertex, firstInstance) {
            var bufferedPayload = createBufferedDrawPayload(attachment, attachmentView, currentPipeline, currentBindGroups, currentVertexBuffers, currentIndexBuffer, {
                drawType: "draw-indexed",
                indexCount: indexCount,
                instanceCount: instanceCount,
                firstIndex: firstIndex,
                baseVertex: baseVertex,
                firstInstance: firstInstance
            });
            if (bufferedPayload && typeof init.onEnd === "function") {
                emittedBufferedDraw = true;
                init.onEnd({
                    type: "native-draw-current-texture-buffered",
                    payload: bufferedPayload
                });
                return;
            }
            if (typeof originalDrawIndexed === "function") {
                return originalDrawIndexed.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.executeBundles = function(bundles) {
            if (!bundles || typeof bundles.length !== "number") {
                if (typeof originalExecuteBundles === "function") {
                    return originalExecuteBundles.apply(encoder, arguments);
                }
                return;
            }
            for (var i = 0; i < bundles.length; ++i) {
                var bundle = bundles[i];
                var commands = bund)__JS__"
R"__JS__(le && bundle._commands ? bundle._commands : [];
                for (var j = 0; j < commands.length; ++j) {
                    var command = commands[j];
                    if (!command) continue;
                    if (command.type === "set-pipeline") {
                        encoder.setPipeline(command.pipeline);
                    } else if (command.type === "set-bind-group") {
                        encoder.setBindGroup(command.index, command.bindGroup);
                    } else if (command.type === "set-vertex-buffer") {
                        encoder.setVertexBuffer(command.slot, command.buffer, command.offset, command.size);
                    } else if (command.type === "set-index-buffer") {
                        encoder.setIndexBuffer(command.buffer, command.format, command.offset, command.size);
                    } else if (command.type === "draw") {
                        encoder.draw(command.vertexCount, command.instanceCount, command.firstVertex, command.firstInstance);
                    } else if (command.type === "draw-indexed") {
                        encoder.drawIndexed(command.indexCount, command.instanceCount, command.firstIndex, command.baseVertex, command.firstInstance);
                    }
                }
            }
        };

        var originalEnd = encoder.end;
        encoder.end = function() {
            if (!emittedBufferedDraw && typeof init.onEnd === "function") {
                var presentPayload = createPresentTexturePayload(attachmentView, currentPipeline, currentBindGroups);
                if (presentPayload) {
                    init.onEnd({
                        type: "native-present-texture-buffered",
                        payload: presentPayload
                    });
                }
            }
            if (typeof originalEnd === "function") {
                return originalEnd.apply(encoder, arguments);
            }
            return undefined;
        };

        return encoder;
    };

    __createMockGPUQueue = function(init) {
        var queue = originalCreateMockGPUQueue(init || {});
        queue.submit = function(commandBuffers) {
            queue._submitCount += commandBuffers && typeof commandBuffers.length === "number" ? commandBuffers.length : 0;
            if (!queue._nativeBridge || !commandBuffers) {
                return;
            }
            for (var i = 0; i < commandBuffers.length; ++i) {
                var commandBuffer = commandBuffers[i];
                var commands = commandBuffer && commandBuffer._commands ? commandBuffer._commands : [];
                for (var j = 0; j < commands.length; ++j) {
                    var command = commands[j];
                    if (!command) continue;
                    if (command.type === "native-clear-current-texture" && typeof __gpuQueueSubmitImpl === "function") {
                        __gpuQueueSubmitImpl(command.canvasId, command.r, command.g, command.b, command.a);
                        continue;
                    }
                    if (command.type === "native-draw-current-texture-buffered" &&
                        typeof __gpuQueueDrawBufferedImpl === "function") {
                        __gpuQueueDrawBufferedImpl(command.payload);
                        continue;
                    }
                    if (command.type === "native-present-texture-buffered" &&
                        typeof __gpuQueuePresentTextureImpl === "function") {
                        __gpuQueuePresentTextureImpl(command.payload);
                    }
                }
            }
        };
        return queue;
    };

    __createMockGPUDevice = function(adapter, descriptor, init) {
        var device = originalCreateMockGPUDevice(adapter, descriptor, init || {});
        device.createRenderBundleEncoder = function(bundleDescriptor) {
            var commands = [];
            return {
                _objectName: "GPURenderBundleEncoder",
                label: bundleDescriptor && bundleDescriptor.label ? bundleDescriptor.label : "",
                setPipeline: function(pipeline) {
                    commands.push({ type: "set-pipeline", pipeline: pipeline || null });
                },
                setBindGroup: function(index, bindGroup) {
                    commands.push({ type: "set-bind-group", index: index == null ? 0 : index, bindGroup: bindGroup || null });
                },
                setVertexBuffer: function(slot, buffer, offset, size) {
                    commands.push({
                        type: "set-vertex-buffer",
                        slot: slot == null ? 0 : slot,
                        buffer: buffer || null,
                        offset: offset == null ? 0 : offset,
                        size: size
                    });
                },
                setIndexBuffer: function(buffer, format, offset, size) {
                    commands.push({
                        type: "set-index-buffer",
                        buffer: buffer || null,
                        format: format || "uint32",
                        offset: offset == null ? 0 : offset,
                        size: size
                    });
                },
                draw: function(vertexCount, instanceCount, firstVertex, firstInstance) {
                    commands.push({
                        type: "draw",
                        vertexCount: vertexCount == null ? 0 : vertexCount,
                        instanceCount: instanceCount == null ? 1 : instanceCount,
                        firstVertex: firstVertex == null ? 0 : firstVertex,
                        firstInstance: firstInstance == null ? 0 : firstInstance
                    });
                },
                drawIndexed: function(indexCount, instanceCount, firstIndex, baseVertex, firstInstance) {
                    commands.push({
                        type: "draw-indexed",
                        indexCount: indexCount == null ? 0 : indexCount,
                        instanceCount: instanceCount == null ? 1 : instanceCount,
                        firstIndex: firstIndex == null ? 0 : firstIndex,
                        baseVertex: baseVertex == null ? 0 : baseVertex,
                        firstInstance: firstInstance == null ? 0 : firstInstance
                    });
                },
                finish: function(finishDescriptor) {
                    return {
                        _objectName: "GPURenderBundle",
                        label: finishDescriptor && finishDescriptor.label ? finishDescriptor.label : "",
                        _commands: commands.slice()
                    };
                }
            };
        };
        return device;
    };

    __installNativeGpuBufferedDrawAugmentation._installed = true;
}

if (typeof __ensurePulpGpuHelpers === "function") {
    var __originalEnsurePulpGpuHelpers = __ensurePulpGpuHelpers;
    __ensurePulpGpuHelpers = function() {
        __originalEnsurePulpGpuHelpers();
        __installNativeGpuBufferedDrawAugmentation._installed = false;
        __installNativeGpuBufferedDrawAugmentation();
    };
}

__installNativeGpuBufferedDrawAugmentation();
)__JS__"
;

} // namespace pulp::view::preludes
