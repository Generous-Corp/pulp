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

    // pulp #1434 Triage #8 — modern CSS color spaces. Figma copy-CSS
    // emits oklch(...) since 2024; v0 / Tailwind ship lab() and lch();
    // Claude Design transition states emit color-mix(...). Spike-quality
    // conversion: oklch / oklab / lch / lab / color() → sRGB hex. Deep
    // wide-gamut path (Skia SkColor4f) tracked separately; this slice
    // keeps the existing hex pipeline for downstream consumers.
    //
    // CSS 4 syntax: space-separated components, optional `/ <alpha>`.
    //   oklch(0.7 0.18 240 / 50%)
    //   oklab(70% -0.05 0.15)
    //   lch(50 80 240)
    //   lab(50 -40 60 / 0.5)
    //   color(srgb 0.5 0.7 0.9)
    //   color(display-p3 1 0.5 0)
    //   color(srgb-linear 0.215861 0.215861 0.215861)
    var modernParsed = _parseModernColor(str);
    if (modernParsed) {
        var mr = Math.round(Math.min(255, Math.max(0, modernParsed.r * 255)));
        var mg = Math.round(Math.min(255, Math.max(0, modernParsed.g * 255)));
        var mb = Math.round(Math.min(255, Math.max(0, modernParsed.b * 255)));
        var ma = Math.round(Math.min(255, Math.max(0, modernParsed.a * 255)));
        return "#" + _hex2(mr) + _hex2(mg) + _hex2(mb) + (ma < 255 ? _hex2(ma) : "");
    }

    return null;
}

// pulp #1434 Triage #8 — modern color-space dispatcher. Returns
// `{ r, g, b, a }` in linear-output [0, 1] sRGB coordinates (gamma-
// encoded), or null on parse failure. The conversion math mirrors the
// CSS Color Module Level 4 reference algorithms (with D50→D65
// chromatic adaptation for CIE Lab/Lch).
function _parseModernColor(str) {
    // Consume leading function name + interior; alpha isolated by `/`.
    var fnMatch = str.match(/^(oklch|oklab|lch|lab|color)\(\s*(.+?)\s*\)$/);
    if (!fnMatch) return null;
    var fn = fnMatch[1];
    var inner = fnMatch[2];

    // Split at `/` for alpha. Alpha is optional.
    var alphaParts = inner.split('/');
    var compStr = alphaParts[0].trim();
    var alpha = 1;
    if (alphaParts.length === 2) {
        alpha = _parseAlphaToken(alphaParts[1].trim());
    }

    var tokens = compStr.split(/\s+/);

    if (fn === 'color') {
        // color(<space> <c1> <c2> <c3>)
        if (tokens.length < 4) return null;
        var space = tokens[0];
        var c1 = _parseNumericToken(tokens[1], 1.0);
        var c2 = _parseNumericToken(tokens[2], 1.0);
        var c3 = _parseNumericToken(tokens[3], 1.0);
        if (c1 === null || c2 === null || c3 === null) return null;
        var rgb;
        if (space === 'srgb') {
            // Components are gamma-encoded sRGB in [0,1].
            rgb = [c1, c2, c3];
        } else if (space === 'srgb-linear') {
            // Linear-light sRGB → gamma-encode.
            rgb = [_srgbGammaEncode(c1), _srgbGammaEncode(c2), _srgbGammaEncode(c3)];
        } else if (space === 'display-p3') {
            // Display-P3 (gamma-encoded) → linear → sRGB linear (matrix)
            // → sRGB gamma. Out-of-gamut values are clamped at the hex
            // boundary; HDR-aware paths can be added when SkColor4f
            // plumbing lands.
            var linP3R = _srgbGammaDecode(c1);
            var linP3G = _srgbGammaDecode(c2);
            var linP3B = _srgbGammaDecode(c3);
            var lin = _displayP3ToLinearSrgb(linP3R, linP3G, linP3B);
            rgb = [_srgbGammaEncode(lin[0]), _srgbGammaEncode(lin[1]), _srgbGammaEncode(lin[2])];
        } else {
            // Unknown / unsupported space — defer.
            return null;
        }
        return { r: rgb[0], g: rgb[1], b: rgb[2], a: alpha };
    }

    if (tokens.length < 3) return null;

    if (fn === 'oklch' || fn === 'oklab') {
        // OKLab L is 0..1 (also accepts 0%..100%); a/b are typically
        // ±0.4. OKLch chroma typically 0..0.4; hue 0..360 deg.
        var L = _parseNumericToken(tokens[0], 1.0); // % maps to 0..1
        if (L === null) return null;
        var oa, ob;
        if (fn === 'oklab') {
            // a/b: % uses ±0.4 (CSS 4 spec); plain numbers pass through.
            oa = _parseNumericToken(tokens[1], 0.4);
            ob = _parseNumericToken(tokens[2], 0.4);
        } else {
            // oklch: chroma % uses 0..0.4; hue deg.
            var C = _parseNumericToken(tokens[1], 0.4);
            var H = _parseAngleDegrees(tokens[2]);
            if (C === null || H === null) return null;
            oa = C * Math.cos(H * Math.PI / 180);
            ob = C * Math.sin(H * Math.PI / 180);
        }
        if (oa === null || ob === null) return null;
        var lin1 = _oklabToLinearSrgb(L, oa, ob);
        return {
            r: _srgbGammaEncode(lin1[0]),
            g: _srgbGammaEncode(lin1[1]),
            b: _srgbGammaEncode(lin1[2]),
            a: alpha,
        };
    }

    if (fn === 'lch' || fn === 'lab') {
        // CIE Lab L is 0..100; a/b ~ ±125; chroma 0..150 typical;
        // hue 0..360.
        var labL = _parseNumericToken(tokens[0], 100.0); // % → 0..100
        if (labL === null) return null;
        var la, lb;
        if (fn === 'lab') {
            la = _parseNumericToken(tokens[1], 125.0);
            lb = _parseNumericToken(tokens[2], 125.0);
        } else {
            var Cc = _parseNumericToken(tokens[1], 150.0);
            var Hh = _parseAngleDegrees(tokens[2]);
            if (Cc === null || Hh === null) return null;
            la = Cc * Math.cos(Hh * Math.PI / 180);
            lb = Cc * Math.sin(Hh * Math.PI / 180);
        }
        if (la === null || lb === null) return null;
        var lin2 = _cieLabToLinearSrgb(labL, la, lb);
        return {
            r: _srgbGammaEncode(lin2[0]),
            g: _srgbGammaEncode(lin2[1]),
            b: _srgbGammaEncode(lin2[2]),
            a: alpha,
        };
    }

    return null;
}

// Numeric component: bare number OR percentage. `pctRange` is the
// 100% mapping (1.0 for OKLab L, 0.4 for OKLab a/b, 100 for CIE Lab L,
// 125 for CIE Lab a/b, etc.). Returns null on parse failure.
function _parseNumericToken(tok, pctRange) {
    if (tok === undefined || tok === null) return null;
    if (tok === 'none') return 0; // CSS 4 `none` → 0 (cheap fallback)
    var t = String(tok).trim();
    if (t.slice(-1) === '%') {
        var pct = parseFloat(t.slice(0, -1));
        return isNaN(pct) ? null : (pct / 100) * pctRange;
    }
    var n = parseFloat(t);
    return isNaN(n) ? null : n;
}

// Alpha can be a bare number (0..1), a percentage (0%..100%), or `none`.
function _parseAlphaToken(tok) {
    if (!tok) return 1;
    if (tok === 'none') return 1;
    if (tok.slice(-1) === '%') {
        var p = parseFloat(tok.slice(0, -1));
        return isNaN(p) ? 1 : Math.max(0, Math.min(1, p / 100));
    }
    var a = parseFloat(tok);
    if (isNaN(a)) return 1;
    return Math.max(0, Math.min(1, a > 1 ? a / 255 : a));
}

// Angle: '240', '240deg', '4.18rad', '0.5turn', '267grad' → degrees.
function _parseAngleDegrees(tok) {
    if (!tok) return null;
    var t = String(tok).trim();
    var m = t.match(/^(-?[\d.]+)(deg|rad|turn|grad)?$/);
    if (!m) return null;
    var n = parseFloat(m[1]);
    if (isNaN(n)) return null;
    var unit = m[2] || 'deg';
    if (unit === 'rad') return n * 180 / Math.PI;
    if (unit === 'turn') return n * 360;
    if (unit === 'grad') return n * 0.9;
    return n;
}

// sRGB gamma encode/decode — CSS-spec piecewise transform.
function _srgbGammaEncode(v) {
    if (v <= 0.0031308) return 12.92 * v;
    return 1.055 * Math.pow(v, 1 / 2.4) - 0.055;
}
function _srgbGammaDecode(v) {
    if (v <= 0.04045) return v / 12.92;
    return Math.pow((v + 0.055) / 1.055, 2.4);
}

// OKLab → linear sRGB (Björn Ottosson's published matrices).
function _oklabToLinearSrgb(L, a, b) {
    var l_ = L + 0.3963377774 * a + 0.2158037573 * b;
    var m_ = L - 0.1055613458 * a - 0.0638541728 * b;
    var s_ = L - 0.0894841775 * a - 1.2914855480 * b;
    var l = l_ * l_ * l_;
    var m = m_ * m_ * m_;
    var s = s_ * s_ * s_;
    return [
        +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    ];
}

// CIE Lab (D50) → linear sRGB. Lab → XYZ (D50) → XYZ (D65, Bradford
// adaptation) → linear sRGB.
function _cieLabToLinearSrgb(L, a, b) {
    // Lab → XYZ (D50 reference white).
    var fy = (L + 16) / 116;
    var fx = a / 500 + fy;
    var fz = fy - b / 200;
    var delta = 6 / 29;
    var d2 = delta * delta;
    var d3 = delta * delta * delta;
    function f1(t) { return t > delta ? t * t * t : 3 * d2 * (t - 4 / 29); }
    var X50 = 0.96422 * f1(fx);
    var Y50 = 1.00000 * f1(fy);
    var Z50 = 0.82521 * f1(fz);
    // D50 → D65 Bradford adaptation (CSS-spec matrix).
    var X65 =  0.9555766 * X50 + -0.0230393 * Y50 +  0.0631636 * Z50;
    var Y65 = -0.0282895 * X50 +  1.0099416 * Y50 +  0.0210077 * Z50;
    var Z65 =  0.0122982 * X50 + -0.0204830 * Y50 +  1.3299098 * Z50;
    // XYZ (D65) → linear sRGB.
    return [
         3.2404542 * X65 + -1.5371385 * Y65 + -0.4985314 * Z65,
        -0.9692660 * X65 +  1.8760108 * Y65 +  0.0415560 * Z65,
         0.0556434 * X65 + -0.2040259 * Y65 +  1.0572252 * Z65,
    ];
}

// Display-P3 (linear) → linear sRGB. P3 → XYZ (D65) → linear sRGB.
function _displayP3ToLinearSrgb(r, g, b) {
    // P3 (D65) → XYZ.
    var X =  0.4865709 * r + 0.2656677 * g + 0.1982173 * b;
    var Y =  0.2289746 * r + 0.6917385 * g + 0.0792869 * b;
    var Z =  0.0000000 * r + 0.0451134 * g + 1.0439443 * b;
    // XYZ → linear sRGB.
    return [
         3.2404542 * X + -1.5371385 * Y + -0.4985314 * Z,
        -0.9692660 * X +  1.8760108 * Y +  0.0415560 * Z,
         0.0556434 * X + -0.2040259 * Y +  1.0572252 * Z,
    ];
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
            // pulp #1434 Triage #9 — handle rad / turn / grad alongside
            // deg. Plain numbers in rotate-family functions are degrees;
            // numeric scale-family / matrix args pass through.
            var ang = _parseTransformAngle(a);
            if (ang !== null) return ang;
            var l = parseCSSLength(a);
            return l ? l.value : parseFloat(a) || 0;
        });
        result.push({ fn: fn, args: args });
    }
    return result;
}

// Parse a CSS angle to degrees. Returns null if the token isn't an
// angle (so callers can fall back to length / numeric parsing).
function _parseTransformAngle(t) {
    if (!t) return null;
    var s = String(t).trim();
    var m = s.match(/^(-?[\d.]+)(deg|rad|turn|grad)$/);
    if (!m) return null;
    var n = parseFloat(m[1]);
    if (isNaN(n)) return null;
    var u = m[2];
    if (u === "deg") return n;
    if (u === "rad") return n * 180 / Math.PI;
    if (u === "turn") return n * 360;
    if (u === "grad") return n * 0.9;
    return null;
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

// Maximum recursion depth for nested var() fallbacks. Generous — real
// design systems top out at depth 2-3. The cap exists only to bound
// pathological self-referential tokens like `--a: var(--a)`; when the
// cap is hit, the walker returns the input string unmodified
// (graceful unresolved passthrough, not silent truncation). Named so
// the cap behavior is grep-able from tests + Codex review on PR C.
var _VAR_RESOLVE_MAX_DEPTH = 8;

// Resolve var(--name) or var(--name, fallback) against theme tokens.
//
// pulp-internal coverage-gap (`css/__var`): the original single-pass regex
// couldn't handle nested parens because regex isn't context-free — the
// `[^)]+` for the fallback would stop at the FIRST `)` (the inner one)
// and leave the outer `)` as part of the surrounding text, producing
// garbage on `var(--a, var(--b, 0))` and similar nested-fallback shapes
// that real design systems emit constantly.
//
// Replaced with a manual balanced-paren walker so:
//   var(--a, var(--b, 0))    →  resolves correctly through fallback chain
//   var(--a) calc(10px)       →  prefix preserved
//   calc(var(--a) + 10px)     →  embedded var() inside other functions OK
function _resolveVar(str, depth) {
    if (!str || typeof str !== "string") return str;
    depth = depth || 0;
    if (depth > _VAR_RESOLVE_MAX_DEPTH) return str;

    var out = "";
    var i = 0;
    while (i < str.length) {
        var varStart = str.indexOf("var(", i);
        if (varStart === -1) {
            out += str.slice(i);
            break;
        }
        out += str.slice(i, varStart);

        // Walk forward tracking paren depth to find the matching `)`.
        var pdepth = 1;
        var j = varStart + 4;
        while (j < str.length && pdepth > 0) {
            var ch = str.charAt(j);
            if (ch === "(") pdepth++;
            else if (ch === ")") pdepth--;
            if (pdepth > 0) j++;
        }
        if (pdepth !== 0) {
            // Unbalanced — surface the remainder verbatim and bail.
            out += str.slice(varStart);
            break;
        }

        // Split inner into name + optional fallback at the FIRST top-level
        // comma (commas inside nested parens stay with the fallback).
        var inner = str.slice(varStart + 4, j);
        var commaPos = -1;
        var nest = 0;
        for (var k = 0; k < inner.length; k++) {
            var c = inner.charAt(k);
            if (c === "(") nest++;
            else if (c === ")") nest--;
            else if (c === "," && nest === 0) { commaPos = k; break; }
        }
        var name, fallback;
        if (commaPos >= 0) {
            name = inner.slice(0, commaPos).trim();
            fallback = inner.slice(commaPos + 1).trim();
        } else {
            name = inner.trim();
            fallback = undefined;
        }
        if (name.indexOf("--") === 0) name = name.slice(2);

        // pulp #1899 (gap #3) — consult the string-token map first so
        // font-family-shaped tokens (`--mono: "JetBrains Mono"`)
        // resolve to the registered string rather than zero. Falls
        // through to the motion-token (numeric) lookup for legacy
        // length/spacing tokens.
        var strVal = (typeof getStringToken === 'function') ? getStringToken(name) : '';
        if (strVal) {
            out += String(strVal);
        } else {
            var val = getMotionToken(name);
            if (val !== 0 && val !== undefined) {
                out += String(val);
            } else if (fallback !== undefined) {
                // Recurse so nested var() in the fallback resolves too.
                out += _resolveVar(fallback, depth + 1);
            } else {
                out += "0";
            }
        }
        i = j + 1;
    }
    return out;
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
