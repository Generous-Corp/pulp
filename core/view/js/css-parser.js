// css-parser.js — CSS value parsing utilities
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
        else if (op2.value === "-") result -= next2.value;
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
