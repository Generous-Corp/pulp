// ═══════════════════════════════════════════════════════════════════════════════
// HTMLCanvasElement + CanvasRenderingContext2D
// ═══════════════════════════════════════════════════════════════════════════════

// pulp #964 — CanvasGradient, returned by createLinearGradient /
// createRadialGradient and assignable to ctx.fillStyle / ctx.strokeStyle.
// Stops are accumulated via addColorStop and flushed to the bridge by
// _applyFillStyle when the gradient is the active style.
function CanvasGradient(kind, params) {
    this._kind = kind;             // "linear" | "radial"
    this._params = params || {};
    this._stops = [];              // [{ offset, color }, ...]
}
CanvasGradient.prototype.addColorStop = function(offset, color) {
    this._stops.push({ offset: Number(offset) || 0, color: String(color || "") });
};

// pulp #1434 bridge-thin gap-fill — CanvasPattern, returned by
// ctx.createPattern(image, repetition) and assignable to ctx.fillStyle /
// ctx.strokeStyle. Repetition values per Canvas2D spec:
//   "repeat" (default), "repeat-x", "repeat-y", "no-repeat"
// Image source is reduced to a path / data URL string the same way
// drawImage normalises it. Flushed to the bridge via canvasSetFillPattern
// when assigned to ctx.fillStyle. The Skia backend renders the real
// tiled pattern via SkShader::MakeImage with SkTileMode::{kRepeat,kDecal};
// the CG backend degrades to flat fill (CG has no first-class pattern
// shader without CGPattern dance — same fallback shape that conic took
// before its real impl landed).
function CanvasPattern(src, tileX, tileY) {
    this._kind = "pattern";
    this._src = String(src || "");
    this._tileX = String(tileX || "repeat");
    this._tileY = String(tileY || "repeat");
}

function CanvasRenderingContext2D(canvasEl) {
    this.canvas = canvasEl;
    this._id = canvasEl._id;
    this.fillStyle = "#000000";
    this.strokeStyle = "#000000";
    this.lineWidth = 1;
    this.font = "14px Inter";
    // pulp #964 — Canvas2D state setters that the bridge accepts via dedicated
    // canvas* functions. Tracked as plain fields and pushed to the bridge on
    // demand by the _sync* helpers below.
    this.textAlign = "left";
    this.textBaseline = "top";
    this.lineCap = "butt";
    this.lineJoin = "miter";
    this.miterLimit = 10;
    this.lineDashOffset = 0;
    this.globalAlpha = 1;
    this.globalCompositeOperation = "source-over";
    this.imageSmoothingEnabled = true;
    this.imageSmoothingQuality = "low";
    // pulp #1520 — Canvas2D ctx.direction. Spec values: "ltr" | "rtl" |
    // "inherit" (we treat "inherit" as the default ltr, matching the
    // spec's "directionality from the canvas element / document"
    // resolution path on a host that doesn't expose a writing-direction
    // computed style yet). Tracked locally so the getter round-trips
    // and flushed to the bridge by `_syncDirectionState` before the
    // next fillText / strokeText.
    this.direction = "ltr";
    // pulp #1520 — Canvas2D ctx.filter. Spec: a CSS <filter-function-list>
    // string applied to subsequent draw operations (blur, brightness,
    // contrast, drop-shadow, grayscale, hue-rotate, invert, opacity,
    // saturate, sepia). The default is "none". Tracked locally and
    // flushed to the bridge by `_syncFilterState` before the next
    // fill/stroke/text/image draw — same caching shape as shadow*.
    this.filter = "none";
    // pulp #1434 batch 7 — Canvas2D drop-shadow state. Each property
    // mirrors the spec defaults: shadow inactive (transparent black,
    // zero blur, zero offset). Tracked locally so getters round-trip
    // (the spec requires the most-recently-assigned value), and
    // flushed to the bridge by `_syncShadowState` before any
    // fill/stroke/text draw — same caching pattern the line / global
    // setters use to avoid redundant bridge spam.
    this.shadowColor = "rgba(0, 0, 0, 0)";
    this.shadowBlur = 0;
    this.shadowOffsetX = 0;
    this.shadowOffsetY = 0;
    this._lineDash = [];
    // _activeFillKind tracks whether the most recently applied fillStyle was
    // a "color" or a "gradient". When a gradient is active the next
    // canvasFillRect / canvasFillPath uses the bridge's active gradient
    // state (pulp #968 use_active_style path) — the shim does NOT call
    // canvasSetFillColor before the draw or the gradient would be
    // overwritten back to a solid colour.
    this._activeFillKind = "color";
    this._activeStrokeKind = "color";
    // Cache of last-pushed font / textAlign / textBaseline / line* / global*
    // state so we don't spam the bridge with redundant set_* commands.
    this._sentFont = null;
    this._sentTextAlign = null;
    this._sentTextBaseline = null;
    this._sentLineCap = null;
    this._sentLineJoin = null;
    // pulp #1434 — track miterLimit and imageSmoothing* sticky state so
    // _syncLineState / _syncImageSmoothingState only push when changed.
    this._sentMiterLimit = null;
    this._sentImageSmoothingEnabled = null;
    this._sentImageSmoothingQuality = null;
    this._sentGlobalAlpha = null;
    this._sentGlobalCompositeOperation = null;
    this._sentShadowColor = null;
    this._sentShadowBlur = null;
    this._sentShadowOffsetX = null;
    this._sentShadowOffsetY = null;
    // pulp #1520 — sticky direction / filter state caches.
    this._sentDirection = null;
    this._sentFilter = null;
    // pulp #1527 — JS-side mirror of the current 2D affine transform,
    // tracked by translate / scale / rotate / setTransform / transform
    // and the save / restore stack. The bridge replays draw commands at
    // paint() time, so the C++ canvas does not have a "current matrix"
    // queryable synchronously from JS. We mirror it here so getTransform
    // can return a DOMMatrix-shaped object without a round-trip. Layout:
    //   [a, b, c, d, e, f]  (matches HTML5 spec / DOMMatrix2DInit)
    //     | a c e |
    //     | b d f |
    //     | 0 0 1 |
    this._currentTransform = [1, 0, 0, 1, 0, 0];
    // Stack of [_currentTransform, _pathSubpaths] snapshots for save/restore.
    this._stateStack = [];
    // pulp #1527 — JS-side mirror of the current path so isPointInPath /
    // isPointInStroke can answer synchronously via a JS hit test. Each
    // subpath is an array of [x, y] points appended by moveTo / lineTo
    // (curve / arc helpers reduce to lineTo / cubicTo segments which we
    // approximate as straight edges between sampled points — matches the
    // bridge's existing `arc` polyline approximation). The bridge owns
    // the canonical SkPath used for fill / stroke / clip; this JS mirror
    // exists only for the synchronous-return query methods.
    this._pathSubpaths = [];
}

// pulp #1527 — DOMMatrix-like return value for getTransform(). The HTML5
// spec returns a `DOMMatrix` instance with `a, b, c, d, e, f` and the
// `is2D` / `isIdentity` flags. `toFloat32Array` / `toFloat64Array` are
// the most-used readers in plugin code (Three.js, Skia-canvas adapters).
// The shim provides a minimal but spec-shaped object — full DOMMatrix
// (3D, multiplications, decomposition) is out-of-scope for the canvas
// bridge layer.
function _PulpCanvasMatrix(a, b, c, d, e, f) {
    this.a = a; this.b = b; this.c = c; this.d = d; this.e = e; this.f = f;
    this.m11 = a; this.m12 = b; this.m21 = c; this.m22 = d;
    this.m41 = e; this.m42 = f;
    // 3D fields (identity for 2D).
    this.m13 = 0; this.m14 = 0; this.m23 = 0; this.m24 = 0;
    this.m31 = 0; this.m32 = 0; this.m33 = 1; this.m34 = 0;
    this.m43 = 0; this.m44 = 1;
    this.is2D = true;
    this.isIdentity = (a === 1 && b === 0 && c === 0 && d === 1
                      && e === 0 && f === 0);
}
_PulpCanvasMatrix.prototype.toFloat32Array = function() {
    return [this.m11, this.m12, this.m13, this.m14,
            this.m21, this.m22, this.m23, this.m24,
            this.m31, this.m32, this.m33, this.m34,
            this.m41, this.m42, this.m43, this.m44];
};
_PulpCanvasMatrix.prototype.toFloat64Array = function() {
    return this.toFloat32Array();
};
_PulpCanvasMatrix.prototype.toJSON = function() {
    return { a: this.a, b: this.b, c: this.c, d: this.d,
             e: this.e, f: this.f, is2D: true, isIdentity: this.isIdentity };
};

CanvasRenderingContext2D.prototype._applyFillStyle = function() {
    var fs = this.fillStyle;
    if (fs && fs._kind === "linear" && typeof canvasSetLinearGradient === "function") {
        var p = fs._params, s = fs._stops;
        var args = [this._id, p.x0, p.y0, p.x1, p.y1];
        for (var i = 0; i < s.length; ++i) { args.push(s[i].color); args.push(s[i].offset); }
        canvasSetLinearGradient.apply(null, args);
        this._activeFillKind = "gradient";
        return;
    }
    if (fs && fs._kind === "radial") {
        var pr = fs._params, sr = fs._stops;
        // pulp #1524 — prefer the two-circle bridge. Both Skia (MakeTwoPointConical)
        // and CG (CGContextDrawRadialGradient with both circles) honour the inner
        // circle. Older binaries without the new bridge fall through to the
        // single-circle outer-only path so JS hot-reload doesn't crash.
        if (typeof canvasSetRadialGradientTwoCircles === "function") {
            var arx = [this._id, pr.x0, pr.y0, pr.r0, pr.x1, pr.y1, pr.r1];
            for (var jj = 0; jj < sr.length; ++jj) { arx.push(sr[jj].color); arx.push(sr[jj].offset); }
            canvasSetRadialGradientTwoCircles.apply(null, arx);
            this._activeFillKind = "gradient";
            return;
        }
        if (typeof canvasSetRadialGradient === "function") {
            var ar = [this._id, pr.x1, pr.y1, pr.r1];
            for (var j = 0; j < sr.length; ++j) { ar.push(sr[j].color); ar.push(sr[j].offset); }
            canvasSetRadialGradient.apply(null, ar);
            this._activeFillKind = "gradient";
            return;
        }
    }
    // pulp #1434 bridge-thin gap-fill — ctx.createConicGradient. Skia
    // routes through SkGradientShader::MakeSweep; CG degrades to the
    // first-stop colour. Same flush shape as linear/radial.
    if (fs && fs._kind === "conic" && typeof canvasSetConicGradient === "function") {
        var pc = fs._params, sc = fs._stops;
        var ac = [this._id, pc.cx, pc.cy, pc.startAngle];
        for (var k = 0; k < sc.length; ++k) { ac.push(sc[k].color); ac.push(sc[k].offset); }
        canvasSetConicGradient.apply(null, ac);
        this._activeFillKind = "gradient";
        return;
    }
    // pulp #1434 bridge-thin gap-fill — ctx.createPattern. Skia: real
    // tiled paint via SkShader::MakeImage with SkTileMode per axis. CG
    // degrades to the active solid colour (no native pattern shader).
    // We reuse `_activeFillKind = "gradient"` as the "non-color" sentinel
    // so canvasClearGradient resets correctly when the next fillStyle
    // assignment is a plain string.
    if (fs && fs._kind === "pattern" && typeof canvasSetFillPattern === "function") {
        canvasSetFillPattern(this._id, fs._src, fs._tileX, fs._tileY);
        this._activeFillKind = "gradient";
        return;
    }
    // Solid colour. Clear any active gradient so subsequent fills don't pick
    // up a stale gradient (Canvas2D spec: assigning fillStyle replaces the
    // previous style outright).
    if (this._activeFillKind === "gradient" && typeof canvasClearGradient === "function") {
        canvasClearGradient(this._id);
    }
    this._activeFillKind = "color";
    if (typeof canvasSetFillColor === "function") canvasSetFillColor(this._id, String(fs == null ? "" : fs));
};

CanvasRenderingContext2D.prototype._applyStrokeStyle = function() {
    var ss = this.strokeStyle;
    // pulp #1434 — CanvasPattern as strokeStyle. If the bridge exposes
    // canvasSetStrokePattern (Skia path), flush the pattern; otherwise
    // fall through to the solid-fallback below (CG path).
    if (ss && ss._kind === "pattern" && typeof canvasSetStrokePattern === "function") {
        canvasSetStrokePattern(this._id, ss._src, ss._tileX, ss._tileY);
        if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, this.lineWidth);
        this._activeStrokeKind = "pattern";
        return;
    }
    // pulp Wave 3 c2d.7 — gradient on strokeStyle. Mirror of
    // _applyFillStyle: prefer the per-kind stroke-gradient bridge fn
    // when present (Skia / future CG impls); fall back to the
    // first-stop solid colour for binaries without the bridge fn so JS
    // hot-reload doesn't crash.
    if (ss && ss._kind === "linear" && typeof canvasSetStrokeLinearGradient === "function") {
        var lp = ss._params, ls = ss._stops;
        var largs = [this._id, lp.x0, lp.y0, lp.x1, lp.y1];
        for (var li = 0; li < ls.length; ++li) { largs.push(ls[li].color); largs.push(ls[li].offset); }
        canvasSetStrokeLinearGradient.apply(null, largs);
        if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, this.lineWidth);
        this._activeStrokeKind = "gradient";
        return;
    }
    if (ss && ss._kind === "radial") {
        var rp = ss._params, rs = ss._stops;
        if (typeof canvasSetStrokeRadialGradientTwoCircles === "function") {
            var rargs = [this._id, rp.x0, rp.y0, rp.r0, rp.x1, rp.y1, rp.r1];
            for (var ri = 0; ri < rs.length; ++ri) { rargs.push(rs[ri].color); rargs.push(rs[ri].offset); }
            canvasSetStrokeRadialGradientTwoCircles.apply(null, rargs);
            if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, this.lineWidth);
            this._activeStrokeKind = "gradient";
            return;
        }
        if (typeof canvasSetStrokeRadialGradient === "function") {
            var sargs = [this._id, rp.x1, rp.y1, rp.r1];
            for (var si = 0; si < rs.length; ++si) { sargs.push(rs[si].color); sargs.push(rs[si].offset); }
            canvasSetStrokeRadialGradient.apply(null, sargs);
            if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, this.lineWidth);
            this._activeStrokeKind = "gradient";
            return;
        }
    }
    if (ss && ss._kind === "conic" && typeof canvasSetStrokeConicGradient === "function") {
        var cp = ss._params, cs = ss._stops;
        var cargs = [this._id, cp.cx, cp.cy, cp.startAngle];
        for (var ci = 0; ci < cs.length; ++ci) { cargs.push(cs[ci].color); cargs.push(cs[ci].offset); }
        canvasSetStrokeConicGradient.apply(null, cargs);
        if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, this.lineWidth);
        this._activeStrokeKind = "gradient";
        return;
    }
    // Fallback: gradient bridge fn missing (older binary), or
    // CanvasPattern with no stroke-pattern bridge. Pull a solid
    // colour — first stop for gradients, neutral grey for unsupported
    // patterns — so strokes still render visibly.
    var colorStr = "";
    if (ss && (ss._kind === "linear" || ss._kind === "radial" || ss._kind === "conic")) {
        colorStr = (ss._stops && ss._stops.length > 0) ? ss._stops[0].color : "#fff";
        this._activeStrokeKind = "gradient";
    } else if (ss && ss._kind === "pattern") {
        colorStr = "#888";
        this._activeStrokeKind = "pattern";
    } else {
        colorStr = String(ss == null ? "" : ss);
        // Solid-colour assignment after a gradient: clear any active
        // stroke shader so the next stroke uses set_stroke_color cleanly
        // (mirrors fillStyle's canvasClearGradient flush).
        if (this._activeStrokeKind === "gradient" &&
            typeof canvasClearStrokeGradient === "function") {
            canvasClearStrokeGradient(this._id);
        }
        this._activeStrokeKind = "color";
    }
    if (typeof canvasSetStrokeColor === "function") canvasSetStrokeColor(this._id, colorStr);
    if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, this.lineWidth);
};

// pulp #1434 — Parse the CSS Fonts Module Level 4 `font` shorthand:
//
//   [<font-style>] [<font-variant>] [<font-weight>] [<font-stretch>]
//   <font-size>[/<line-height>] <font-family>
//
// where <font-size> is the one mandatory token and <font-family> the
// other (everything else is optional, can appear in any order before
// size, and any number of leading tokens can be `normal`).
//
// This is the canonical Figma copy-CSS shape, e.g.
//
//   ctx.font = "italic small-caps bold 14px/1.4 'Inter', sans-serif";
//
// Returns an object:
//
//   {
//     family:      "Inter, sans-serif",
//     size:        14,                  // px
//     weight:      700,                 // 100..900 (CSS keyword → number)
//     slant:       1,                   // 0=upright, 1=italic/oblique
//     variant:     "small-caps",        // tracked but not yet plumbed
//     lineHeight:  1.4,                 // null when omitted; not plumbed
//     letterSpacing: 0                  // shorthand has no letter-spacing
//   }
//
// Unknown tokens are silently dropped — matches browser behaviour where
// the entire shorthand is rejected on a hard parse error, but ours is a
// best-effort parser tuned for real-world copy-CSS values.
//
// Exposed as a static helper so both _syncTextState and measureText can
// share the parse without round-tripping the regex twice.
CanvasRenderingContext2D._parseFontShorthand = function(fontStr) {
    var out = {
        family: "Inter",
        size: 14,
        weight: 400,
        slant: 0,
        variant: "normal",
        lineHeight: null,
        letterSpacing: 0
    };
    if (!fontStr || typeof fontStr !== "string") return out;
    var s = fontStr.trim();
    if (!s) return out;

    // Locate the size token. The size token is the first whitespace-
    // separated token that begins with a digit (or `.`) and ends in a
    // CSS length unit (`px`, `pt`, `em`, `rem`).
    //
    // Match `<size><unit>` optionally followed by `/<line-height>` (a
    // number or a length). After the match, everything before is the
    // optional leading-token list, everything after is the family list.
    var sizeRegex = /(^|\s)(\d+(?:\.\d+)?)(px|pt|em|rem)(?:\s*\/\s*([\d.]+(?:px|pt|em|rem|%)?|normal))?(?=\s|$)/i;
    var m = s.match(sizeRegex);
    if (!m) {
        // No `<size><unit>` token — treat the whole string as a family
        // list, keep the default 14 size.
        out.family = s.replace(/^["']|["']$/g, "");
        return out;
    }
    var sizeNum = parseFloat(m[2]);
    // pulp #1434 P2 — convert non-px units to px so values like
    // `1.2em Inter`, `12pt Inter`, `1rem Inter` produce sane sizes
    // instead of being treated as `1.2px / 12px / 1px`. Canvas2D has no
    // DOM cascade, so em/rem resolve against a fixed 16px root — same
    // default that browsers use at the document root and what every
    // headless Canvas2D shim (jsdom, node-canvas) uses.
    //   px  → as-is
    //   pt  → * (4/3)         (1pt = 1/72in = 4/3 px at 96dpi)
    //   em  → * 16            (no inherited font-size in canvas)
    //   rem → * 16            (no document root in canvas)
    var sizeUnit = (m[3] || "px").toLowerCase();
    if (sizeUnit === "pt")       sizeNum *= 4 / 3;
    else if (sizeUnit === "em")  sizeNum *= 16;
    else if (sizeUnit === "rem") sizeNum *= 16;
    if (isFinite(sizeNum) && sizeNum > 0) out.size = sizeNum;
    if (m[4]) {
        // Line-height: either a unitless number, a length, a %, or `normal`.
        var lh = String(m[4]);
        if (lh === "normal") {
            out.lineHeight = null;
        } else {
            var lhNum = parseFloat(lh);
            if (isFinite(lhNum) && lhNum > 0) out.lineHeight = lhNum;
        }
    }

    var sizeStart = m.index + (m[1] ? m[1].length : 0);
    var sizeEnd   = m.index + m[0].length;
    var leading = s.substring(0, sizeStart).trim();
    var family  = s.substring(sizeEnd).trim();
    if (family) {
        // Strip leading/trailing surrounding quotes from a single-family
        // string (`"Inter"` → `Inter`); preserve quoted entries inside a
        // multi-family list verbatim because the bridge takes the family
        // string as-is and the OS font lookup tolerates either form.
        if (family.indexOf(",") < 0) {
            family = family.replace(/^["']|["']$/g, "");
        }
        out.family = family;
    }

    // Walk the leading tokens (style / variant / weight / stretch). Each
    // is whitespace-separated; bare keywords map to known buckets, anything
    // numeric maps to weight.
    if (leading) {
        var tokens = leading.split(/\s+/);
        for (var i = 0; i < tokens.length; ++i) {
            var t = tokens[i].toLowerCase();
            if (!t || t === "normal") continue;
            // Style
            if (t === "italic" || t === "oblique") { out.slant = 1; continue; }
            // Variant
            if (t === "small-caps") { out.variant = "small-caps"; continue; }
            // Weight (keyword → numeric)
            if (t === "bold")    { out.weight = 700; continue; }
            if (t === "bolder")  { out.weight = 700; continue; }
            if (t === "lighter") { out.weight = 300; continue; }
            // Weight (numeric 100..900)
            if (/^\d{3}$/.test(t)) {
                var w = parseInt(t, 10);
                if (w >= 100 && w <= 900) { out.weight = w; continue; }
            }
            // Stretch keywords — accepted but currently dropped (no
            // bridge plumbing); same fate as variant. Listed explicitly
            // so we don't fall into the "treat as family" trap.
            if (t === "ultra-condensed" || t === "extra-condensed" ||
                t === "condensed" || t === "semi-condensed" ||
                t === "semi-expanded" || t === "expanded" ||
                t === "extra-expanded" || t === "ultra-expanded") {
                continue;
            }
            // Unknown token: silently dropped — see header comment.
        }
    }
    return out;
};

// pulp #964 — push state-setter values to the bridge before any draw
// that depends on them. Cheap (only sends what changed) and idempotent.
CanvasRenderingContext2D.prototype._syncTextState = function() {
    if (this._sentFont !== this.font) {
        var parsed = CanvasRenderingContext2D._parseFontShorthand(
            this.font || "14px Inter");
        // Stash the parsed line-height + variant for measureText round-tripping
        // and for any future bridge plumbing (CSS line-height is currently a
        // shim-side concern; variant has no canvas-API surface yet).
        this._parsedLineHeight = parsed.lineHeight;
        this._parsedFontVariant = parsed.variant;
        // Prefer the rich bridge fn when the host registered it (canvas
        // widgets only — see widget_bridge.cpp). Falls back to the legacy
        // canvasSetFont(id, family, size) on hosts that pre-date pulp #1434.
        if (typeof canvasSetFontFull === "function") {
            canvasSetFontFull(this._id, parsed.family, parsed.size,
                              parsed.weight, parsed.slant,
                              parsed.letterSpacing);
        } else if (typeof canvasSetFont === "function") {
            canvasSetFont(this._id, parsed.family, parsed.size);
        }
        this._sentFont = this.font;
    }
    if (this._sentTextAlign !== this.textAlign) {
        if (typeof canvasSetTextAlign === "function") canvasSetTextAlign(this._id, this.textAlign);
        this._sentTextAlign = this.textAlign;
    }
    if (this._sentTextBaseline !== this.textBaseline) {
        if (typeof canvasSetTextBaseline === "function") canvasSetTextBaseline(this._id, this.textBaseline);
        this._sentTextBaseline = this.textBaseline;
    }
};
CanvasRenderingContext2D.prototype._syncLineState = function() {
    if (this._sentLineCap !== this.lineCap) {
        if (typeof canvasSetLineCap === "function") canvasSetLineCap(this._id, this.lineCap);
        this._sentLineCap = this.lineCap;
    }
    if (this._sentLineJoin !== this.lineJoin) {
        if (typeof canvasSetLineJoin === "function") canvasSetLineJoin(this._id, this.lineJoin);
        this._sentLineJoin = this.lineJoin;
    }
    // pulp #1434 bridge-thin gap-fill — push ctx.miterLimit to the
    // bridge so SkPaint::setStrokeMiter / CGContextSetMiterLimit
    // honour the JS value. Spec: ignore non-finite / non-positive.
    var ml = +this.miterLimit;
    if (isFinite(ml) && ml > 0 && this._sentMiterLimit !== ml) {
        if (typeof canvasSetMiterLimit === "function") canvasSetMiterLimit(this._id, ml);
        this._sentMiterLimit = ml;
    }
};

// pulp #1434 bridge-thin gap-fill — flush ctx.imageSmoothingEnabled and
// ctx.imageSmoothingQuality before the next drawImage. Sticky on the C++
// side; we only push when either field changes.
CanvasRenderingContext2D.prototype._syncImageSmoothingState = function() {
    var en = !!this.imageSmoothingEnabled;
    var q = String(this.imageSmoothingQuality || "low");
    if (q !== "low" && q !== "medium" && q !== "high") q = "low";
    if (this._sentImageSmoothingEnabled !== en
        || this._sentImageSmoothingQuality !== q) {
        if (typeof canvasSetImageSmoothing === "function") {
            canvasSetImageSmoothing(this._id, en, q);
        }
        this._sentImageSmoothingEnabled = en;
        this._sentImageSmoothingQuality = q;
    }
};
CanvasRenderingContext2D.prototype._syncGlobalState = function() {
    if (this._sentGlobalAlpha !== this.globalAlpha) {
        if (typeof canvasSetGlobalAlpha === "function") canvasSetGlobalAlpha(this._id, this.globalAlpha);
        this._sentGlobalAlpha = this.globalAlpha;
    }
    if (this._sentGlobalCompositeOperation !== this.globalCompositeOperation) {
        if (typeof canvasGlobalCompositeOperation === "function") {
            canvasGlobalCompositeOperation(this._id, this.globalCompositeOperation);
        } else if (typeof canvasSetBlendMode === "function") {
            canvasSetBlendMode(this._id, this.globalCompositeOperation);
        }
        this._sentGlobalCompositeOperation = this.globalCompositeOperation;
    }
};

// pulp #1434 batch 7 — flush Canvas2D shadow state to the bridge before
// any fill/stroke/text draw. Sticky on the C++ side, so we only push
// changed values. HTML5 spec: assigning a non-finite number must be
// silently ignored; numeric coercion (`+x` for any value) returns NaN
// for non-numerics which we treat as "no change" so getter round-trip
// still reflects the latest valid value.
CanvasRenderingContext2D.prototype._syncShadowState = function() {
    if (this._sentShadowColor !== this.shadowColor) {
        if (typeof canvasSetShadowColor === "function") {
            canvasSetShadowColor(this._id, String(this.shadowColor || "rgba(0,0,0,0)"));
        }
        this._sentShadowColor = this.shadowColor;
    }
    var b = +this.shadowBlur;
    if (isFinite(b) && b >= 0 && this._sentShadowBlur !== b) {
        if (typeof canvasSetShadowBlur === "function") canvasSetShadowBlur(this._id, b);
        this._sentShadowBlur = b;
    }
    var ox = +this.shadowOffsetX;
    if (isFinite(ox) && this._sentShadowOffsetX !== ox) {
        if (typeof canvasSetShadowOffsetX === "function") canvasSetShadowOffsetX(this._id, ox);
        this._sentShadowOffsetX = ox;
    }
    var oy = +this.shadowOffsetY;
    if (isFinite(oy) && this._sentShadowOffsetY !== oy) {
        if (typeof canvasSetShadowOffsetY === "function") canvasSetShadowOffsetY(this._id, oy);
        this._sentShadowOffsetY = oy;
    }
};

// pulp #1520 — flush ctx.direction to the bridge. Spec values:
//   "ltr"     → 0 (default; matches SkShaper leftToRight=true)
//   "rtl"     → 1 (SkShaper leftToRight=false; HarfBuzz buffer dir RTL)
//   "inherit" → 2 (treated as 0 on backends without a per-View writing
//                  direction; the Skia backend leaves the default ltr
//                  in place, so visually identical to "ltr" for now)
// Unknown strings coerce to "ltr" silently — same shape as
// imageSmoothingQuality's defensive coercion.
CanvasRenderingContext2D.prototype._syncDirectionState = function() {
    var d = String(this.direction || "ltr");
    if (d !== "ltr" && d !== "rtl" && d !== "inherit") d = "ltr";
    if (this._sentDirection === d) return;
    if (typeof canvasSetDirection === "function") {
        var enumVal = (d === "rtl") ? 1 : (d === "inherit") ? 2 : 0;
        canvasSetDirection(this._id, enumVal);
    }
    this._sentDirection = d;
};

// pulp #1520 — flush ctx.filter to the bridge. The spec accepts a
// <filter-function-list> string ("blur(5px) sepia(80%) ...") plus the
// keyword "none". The bridge stashes the raw string; the Skia backend
// parses it into an SkImageFilter chain (blur, grayscale, sepia,
// brightness, contrast, invert, opacity, saturate, hue-rotate) and
// applies via SkPaint::setImageFilter on subsequent draws. Backends
// that don't recognise a particular function silently degrade.
//
// Unlike the CSS `filter` property on a View (#1503), this filter is
// per-2D-context state and stacks with save() / restore().
CanvasRenderingContext2D.prototype._syncFilterState = function() {
    var f = String(this.filter == null ? "none" : this.filter);
    if (this._sentFilter === f) return;
    if (typeof canvasSetFilter === "function") {
        canvasSetFilter(this._id, f);
    }
    this._sentFilter = f;
};

CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._applyFillStyle();
    // pulp #964 — the bridge function is `canvasRect`, NOT `canvasFillRect`.
    // The 5-arg form (no color) honours the active fillStyle / gradient via
    // pulp #968's use_active_style path on the C++ side. Calling `canvasRect`
    // is correct; the previously-dead `canvasFillRect` reference silently
    // dropped every `ctx.fillRect()` call (the typeof guard hid the typo).
    if (typeof canvasRect === "function") canvasRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.strokeRect = function(x, y, w, h) {
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._syncLineState();
    this._applyStrokeStyle();
    if (typeof canvasStrokeRect === "function") canvasStrokeRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.clearRect = function(x, y, w, h) {
    if (typeof canvasClearRect === "function") canvasClearRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.beginPath = function() {
    if (typeof canvasBeginPath === "function") canvasBeginPath(this._id);
    // pulp #1527 — reset the JS-side path mirror so isPointInPath only
    // sees the new path's geometry. Bridge canvasBeginPath does the same
    // on the C++ side.
    this._pathSubpaths = [];
};

CanvasRenderingContext2D.prototype.moveTo = function(x, y) {
    if (typeof canvasMoveTo === "function") canvasMoveTo(this._id, x, y);
    // pulp #1527 — open a new subpath on the JS mirror. moveTo always
    // starts a fresh subpath per the HTML5 path-construction spec.
    this._pathSubpaths.push([[+x, +y]]);
};

CanvasRenderingContext2D.prototype.lineTo = function(x, y) {
    if (typeof canvasLineTo === "function") canvasLineTo(this._id, x, y);
    // pulp #1527 — append to the current subpath. Spec: if no subpath
    // exists, lineTo behaves as moveTo (HTML5 §canvas-2d step 1).
    if (this._pathSubpaths.length === 0) {
        this._pathSubpaths.push([[+x, +y]]);
    } else {
        this._pathSubpaths[this._pathSubpaths.length - 1].push([+x, +y]);
    }
};

CanvasRenderingContext2D.prototype.closePath = function() {
    if (typeof canvasClosePath === "function") canvasClosePath(this._id);
    // pulp #1527 — append the first point to close the loop. Spec: a
    // closed subpath behaves like an additional segment back to the
    // start, which point-in-polygon hit tests handle automatically when
    // the polygon is non-self-intersecting.
    var subs = this._pathSubpaths;
    if (subs.length > 0) {
        var last = subs[subs.length - 1];
        if (last.length > 0) {
            var first = last[0];
            last.push([first[0], first[1]]);
        }
    }
};

CanvasRenderingContext2D.prototype.fill = function(fillRule) {
    // pulp DIVERGE→PASS sweep — fillRule arg ('evenodd' | 'nonzero')
    // threads through to the bridge. Default 'nonzero' = 0; 'evenodd' = 1.
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._applyFillStyle();
    var rule = (fillRule === "evenodd") ? 1 : 0;
    if (typeof canvasFillPath === "function") canvasFillPath(this._id, rule);
};

CanvasRenderingContext2D.prototype.stroke = function() {
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._syncLineState();
    this._applyStrokeStyle();
    if (typeof canvasStrokePath === "function") canvasStrokePath(this._id);
};

// ── pulp #964 — Canvas2D state-stack methods (save/restore) ───────────────
// FilterBank and most non-trivial Canvas2D code uses save()/restore() to
// scope transforms and clip regions per draw subroutine. Without these
// shims, ctx.save() is undefined and the very first call throws TypeError,
// aborting the entire frame render. Once aborted, none of the subsequent
// drawing commands record to the bridge — which is why the FilterBank repro
// for #964 saw an empty canvas even though the early commands like
// clearRect / setStrokeColor showed up in the dispatch log.
CanvasRenderingContext2D.prototype.save = function() {
    if (typeof canvasSave === "function") canvasSave(this._id);
    // pulp #1527 — push the current JS-mirrored transform + path snapshot
    // so getTransform / isPointInPath stay correct across save/restore.
    // The bridge's save() captures the C++-side state; we capture the
    // JS-side mirror here. Cloning protects against later mutation of
    // the live arrays inside `_currentTransform` and `_pathSubpaths`.
    var clonedSubpaths = [];
    for (var sp = 0; sp < this._pathSubpaths.length; ++sp) {
        clonedSubpaths.push(this._pathSubpaths[sp].slice());
    }
    this._stateStack.push({
        transform: this._currentTransform.slice(),
        subpaths: clonedSubpaths
    });
    // Locally invalidate the "what we've already pushed to the bridge"
    // cache so the next state-using draw re-pushes (the bridge's save()
    // captures these on the C++ side, but our JS-side shim doesn't know
    // what was active across save/restore boundaries).
    this._sentFont = this._sentTextAlign = this._sentTextBaseline = null;
    this._sentLineCap = this._sentLineJoin = null;
    this._sentGlobalAlpha = this._sentGlobalCompositeOperation = null;
    this._sentShadowColor = this._sentShadowBlur = null;
    this._sentShadowOffsetX = this._sentShadowOffsetY = null;
    this._sentDirection = this._sentFilter = null;
};

CanvasRenderingContext2D.prototype.restore = function() {
    if (typeof canvasRestore === "function") canvasRestore(this._id);
    // pulp #1527 — pop the JS-mirrored transform + path snapshot. Spec:
    // restoring with no matching save is a no-op (we leave the live
    // state intact in that case rather than clearing it).
    if (this._stateStack.length > 0) {
        var snap = this._stateStack.pop();
        this._currentTransform = snap.transform;
        this._pathSubpaths = snap.subpaths;
    }
    this._sentFont = this._sentTextAlign = this._sentTextBaseline = null;
    this._sentLineCap = this._sentLineJoin = null;
    this._sentGlobalAlpha = this._sentGlobalCompositeOperation = null;
    this._sentShadowColor = this._sentShadowBlur = null;
    this._sentShadowOffsetX = this._sentShadowOffsetY = null;
    this._sentDirection = this._sentFilter = null;
};

// ── pulp #964 — Canvas2D transform methods ────────────────────────────────
//
// Each mutator updates the JS-mirrored `_currentTransform` (pulp #1527)
// in addition to forwarding to the bridge so getTransform() can return
// the live matrix without a round-trip. Composition rules:
//   translate(x,y):  M' = M * T(x,y)   →  e += a*x + c*y; f += b*x + d*y
//   scale(sx,sy):    M' = M * S(sx,sy) →  a *= sx; b *= sx; c *= sy; d *= sy
//   rotate(theta):   M' = M * R(theta) →  cos/sin block applied to (a,b,c,d)
//   setTransform:    replace
//   transform:       M' = M * given (concat-on-right; mirrored locally
//                    even though the bridge only forwards translation).
CanvasRenderingContext2D.prototype.translate = function(x, y) {
    if (typeof canvasTranslate === "function") canvasTranslate(this._id, x, y);
    var t = this._currentTransform;
    t[4] += t[0] * x + t[2] * y;
    t[5] += t[1] * x + t[3] * y;
};
CanvasRenderingContext2D.prototype.scale = function(sx, sy) {
    if (typeof canvasScale === "function") canvasScale(this._id, sx, sy);
    var t = this._currentTransform;
    t[0] *= sx; t[1] *= sx;
    t[2] *= sy; t[3] *= sy;
};
CanvasRenderingContext2D.prototype.rotate = function(radians) {
    if (typeof canvasRotate === "function") canvasRotate(this._id, radians);
    var t = this._currentTransform;
    var co = Math.cos(radians), si = Math.sin(radians);
    var a = t[0], b = t[1], c = t[2], d = t[3];
    t[0] = a * co + c * si;
    t[1] = b * co + d * si;
    t[2] = -a * si + c * co;
    t[3] = -b * si + d * co;
};
CanvasRenderingContext2D.prototype.setTransform = function(a, b, c, d, e, f) {
    // CanvasRenderingContext2D.setTransform also accepts a single DOMMatrix
    // argument (setTransform(matrix)). Detect that form and unpack.
    if (arguments.length === 1 && a && typeof a === "object") {
        var m = a;
        b = m.b == null ? 0 : m.b;
        c = m.c == null ? 0 : m.c;
        d = m.d == null ? 1 : m.d;
        e = m.e == null ? 0 : m.e;
        f = m.f == null ? 0 : m.f;
        a = m.a == null ? 1 : m.a;
    }
    if (typeof canvasSetTransform === "function") canvasSetTransform(this._id, a, b, c, d, e, f);
    this._currentTransform = [a, b, c, d, e, f];
};
CanvasRenderingContext2D.prototype.resetTransform = function() {
    if (typeof canvasSetTransform === "function") canvasSetTransform(this._id, 1, 0, 0, 1, 0, 0);
    this._currentTransform = [1, 0, 0, 1, 0, 0];
};
// pulp #1527 — getTransform() returns a DOMMatrix-shaped object reflecting
// the current 2D affine transform. HTML5 spec: returns a NEW DOMMatrix
// each call (mutating the returned object must not affect the live ctx
// transform), so we copy the live array into the matrix constructor.
CanvasRenderingContext2D.prototype.getTransform = function() {
    var t = this._currentTransform;
    return new _PulpCanvasMatrix(t[0], t[1], t[2], t[3], t[4], t[5]);
};
// transform(a,b,c,d,e,f) — multiply the current transform by the given
// matrix (concat-on-right). The bridge currently exposes setTransform
// (replace) but not concat for the canvas widget. For the common case of
// a pure translation we forward to canvasTranslate; otherwise this is a
// no-op (file a follow-up if a future plugin needs strict concat).
//
// pulp #1527 — even though the bridge only honours pure-translation, we
// mirror the full concat into _currentTransform so getTransform() returns
// the matrix the spec expects. The visual rendering still degrades for
// non-translation matrices (caller should not rely on visual output) but
// the query result is correct.
CanvasRenderingContext2D.prototype.transform = function(a, b, c, d, e, f) {
    if (a === 1 && b === 0 && c === 0 && d === 1 && typeof canvasTranslate === "function") {
        canvasTranslate(this._id, e, f);
    }
    // M' = M * given (concat-on-right). Spec semantics for the JS-side
    // mirror; the bridge replay path is the visual lossy one.
    var t = this._currentTransform;
    var na = t[0] * a + t[2] * b;
    var nb = t[1] * a + t[3] * b;
    var nc = t[0] * c + t[2] * d;
    var nd = t[1] * c + t[3] * d;
    var ne = t[0] * e + t[2] * f + t[4];
    var nf = t[1] * e + t[3] * f + t[5];
    this._currentTransform = [na, nb, nc, nd, ne, nf];
};

// ── pulp #1527 — internal JS-side path-mirror helpers ────────────────────
// The bridge owns the canonical SkPath; these helpers maintain a parallel
// JS-side polyline approximation used by isPointInPath / isPointInStroke.
// Curve segments (cubic / quadratic / arc) are sampled into ~16 line
// segments — accurate enough for spec-compliant hit testing without the
// cost of pulling a real bezier solver into JS.
CanvasRenderingContext2D.prototype._pathMirrorMoveTo = function(x, y) {
    this._pathSubpaths.push([[+x, +y]]);
};
CanvasRenderingContext2D.prototype._pathMirrorLineTo = function(x, y) {
    if (this._pathSubpaths.length === 0) {
        this._pathSubpaths.push([[+x, +y]]);
    } else {
        this._pathSubpaths[this._pathSubpaths.length - 1].push([+x, +y]);
    }
};
CanvasRenderingContext2D.prototype._pathMirrorLastPoint = function() {
    var subs = this._pathSubpaths;
    if (subs.length === 0) return null;
    var last = subs[subs.length - 1];
    if (last.length === 0) return null;
    return last[last.length - 1];
};
CanvasRenderingContext2D.prototype._pathMirrorCubic = function(c1x, c1y, c2x, c2y, x, y) {
    var p0 = this._pathMirrorLastPoint();
    if (!p0) { this._pathMirrorMoveTo(x, y); return; }
    var x0 = p0[0], y0 = p0[1];
    var STEPS = 16;
    for (var i = 1; i <= STEPS; ++i) {
        var t = i / STEPS;
        var u = 1 - t;
        var bx = u*u*u*x0 + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*x;
        var by = u*u*u*y0 + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*y;
        this._pathMirrorLineTo(bx, by);
    }
};
CanvasRenderingContext2D.prototype._pathMirrorQuad = function(cx, cy, x, y) {
    var p0 = this._pathMirrorLastPoint();
    if (!p0) { this._pathMirrorMoveTo(x, y); return; }
    var x0 = p0[0], y0 = p0[1];
    var STEPS = 16;
    for (var i = 1; i <= STEPS; ++i) {
        var t = i / STEPS;
        var u = 1 - t;
        var bx = u*u*x0 + 2*u*t*cx + t*t*x;
        var by = u*u*y0 + 2*u*t*cy + t*t*y;
        this._pathMirrorLineTo(bx, by);
    }
};

// ── pulp #964 — Path methods (arc / rect / curves) ────────────────────────
// pulp #1521 — replace bezier/lineTo approximations with native bridge calls
// that map to SkPath::arcTo (Skia) and CGPathAddArc (CG). The native arc is
// closed-form correct for full circles, half circles, and the
// 3-collinear-points degenerate case — bezier approximations were ~1px off
// at large radii and lost the exact-tangent property completely.
CanvasRenderingContext2D.prototype.arc = function(cx, cy, radius, startAngle, endAngle, anticlockwise) {
    if (typeof canvasPathArc !== "function") return;
    canvasPathArc(this._id, cx, cy, radius, startAngle, endAngle,
                  anticlockwise ? 1 : 0);
};

CanvasRenderingContext2D.prototype.arcTo = function(x1, y1, x2, y2, radius) {
    if (typeof canvasPathArcTo !== "function") return;
    canvasPathArcTo(this._id, x1, y1, x2, y2, radius);
};

CanvasRenderingContext2D.prototype.bezierCurveTo = function(c1x, c1y, c2x, c2y, x, y) {
    if (typeof canvasCubicTo === "function") canvasCubicTo(this._id, c1x, c1y, c2x, c2y, x, y);
    this._pathMirrorCubic(c1x, c1y, c2x, c2y, x, y);
};

CanvasRenderingContext2D.prototype.quadraticCurveTo = function(cx, cy, x, y) {
    if (typeof canvasQuadTo === "function") canvasQuadTo(this._id, cx, cy, x, y);
    this._pathMirrorQuad(cx, cy, x, y);
};

CanvasRenderingContext2D.prototype.rect = function(x, y, w, h) {
    // rect() is a path-construction op (not a draw). Emit four lineTos
    // back to the start point so the resulting subpath behaves like a
    // closed rectangle for fill()/stroke()/clip().
    if (typeof canvasMoveTo !== "function" || typeof canvasLineTo !== "function") return;
    canvasMoveTo(this._id, x, y);
    canvasLineTo(this._id, x + w, y);
    canvasLineTo(this._id, x + w, y + h);
    canvasLineTo(this._id, x, y + h);
    canvasLineTo(this._id, x, y);
    this._pathMirrorMoveTo(x, y);
    this._pathMirrorLineTo(x + w, y);
    this._pathMirrorLineTo(x + w, y + h);
    this._pathMirrorLineTo(x, y + h);
    this._pathMirrorLineTo(x, y);
};

CanvasRenderingContext2D.prototype.ellipse = function(cx, cy, rx, ry, rotation, startAngle, endAngle, anticlockwise) {
    // pulp #1521 — native ellipse via SkPath::arcTo + SkMatrix rotation
    // (Skia) or CGPathAddArc through a CGAffineTransform (CG). Honours
    // `rotation` correctly; the previous shim ignored it entirely.
    if (typeof canvasPathEllipse !== "function") return;
    canvasPathEllipse(this._id, cx, cy, rx, ry, rotation || 0,
                      startAngle, endAngle, anticlockwise ? 1 : 0);
};

CanvasRenderingContext2D.prototype.roundRect = function(x, y, w, h, radii) {
    // pulp #1521 — native per-corner roundRect via SkRRect::MakeRectRadii
    // (Skia) or 8-segment CGPath layout (CG). The CSS spec accepts radii
    // in five forms — number, [r], [r1, r2], [r1, r2, r3], [r1, r2, r3,
    // r4] — and each can independently be a number (x==y) or an
    // {x, y} object for an elliptical corner. Normalize all forms to
    // 8 floats (tl_x, tl_y, tr_x, tr_y, br_x, br_y, bl_x, bl_y) before
    // crossing the bridge so the C++ side stays narrow.
    if (typeof canvasPathRoundRect !== "function") return;
    function radiusXY(r) {
        if (r == null) return [0, 0];
        if (typeof r === "number") return [r, r];
        if (typeof r === "object") {
            var rx = Number(r.x) || 0;
            var ry = Number(r.y) || 0;
            return [rx, ry];
        }
        return [0, 0];
    }
    var tl, tr, br, bl;
    if (radii == null) {
        tl = tr = br = bl = [0, 0];
    } else if (typeof radii === "number" || (typeof radii === "object" && !Array.isArray(radii))) {
        tl = tr = br = bl = radiusXY(radii);
    } else if (Array.isArray(radii)) {
        if (radii.length === 0) {
            tl = tr = br = bl = [0, 0];
        } else if (radii.length === 1) {
            tl = tr = br = bl = radiusXY(radii[0]);
        } else if (radii.length === 2) {
            // [horizontal, vertical] — corners alternate; per CSS spec
            // [r1, r2] sets top-left/bottom-right to r1, top-right/
            // bottom-left to r2.
            tl = br = radiusXY(radii[0]);
            tr = bl = radiusXY(radii[1]);
        } else if (radii.length === 3) {
            tl = radiusXY(radii[0]);
            tr = bl = radiusXY(radii[1]);
            br = radiusXY(radii[2]);
        } else { // 4+
            tl = radiusXY(radii[0]);
            tr = radiusXY(radii[1]);
            br = radiusXY(radii[2]);
            bl = radiusXY(radii[3]);
        }
    } else {
        tl = tr = br = bl = [0, 0];
    }
    canvasPathRoundRect(this._id, x, y, w, h,
                        tl[0], tl[1],
                        tr[0], tr[1],
                        br[0], br[1],
                        bl[0], bl[1]);
};

CanvasRenderingContext2D.prototype.clip = function(fillRule) {
    // pulp #964 — match Canvas2D's clip() spec: intersect the current
    // clip region with the current path. The bridge's canvasClip
    // (issue-896) calls SkCanvas::clipPath; canvasClipRect is the older
    // rect-only path. Prefer canvasClip when available.
    // pulp DIVERGE→PASS sweep — thread the fillRule arg ('evenodd' |
    // 'nonzero') through to canvasClip, matching what the bridge now
    // accepts as int_val (0 = nonzero, 1 = evenodd).
    var rule = (fillRule === "evenodd") ? 1 : 0;
    if (typeof canvasClip === "function") canvasClip(this._id, rule);
};

// ── pulp #1527 — isPointInPath / isPointInStroke ─────────────────────────
//
// Synchronous-return queries that hit-test the current path against an
// (x, y) point in path-coordinate space. The HTML5 spec also accepts
// an optional fillRule ("nonzero" | "evenodd") and an optional Path2D
// object as the first argument; we implement the common 2-arg form
// (fillRule defaults to "nonzero") and ignore Path2D since the shim
// doesn't yet model standalone Path2D instances. The point is treated
// as already in path-coordinate space (which is what Three.js / Skia
// adapters pass) — full HTML5 semantics would un-transform the point
// via the inverse of `_currentTransform` first, but the path mirror is
// itself recorded in path-coordinate space (matching the bridge), so
// no inverse-transform is needed for our common use cases.
//
// Algorithm: even-odd ray cast — count the number of polygon edges a
// horizontal ray from (x, y) to +∞ intersects. Odd = inside. The
// "nonzero" fillRule additionally tracks edge winding direction; we
// return the same answer for nonzero and evenodd on simple, non-self-
// intersecting paths (the FilterBank / synth UI use case). Plugins that
// need true winding-number semantics on self-intersecting paths can
// file a follow-up.
CanvasRenderingContext2D.prototype._pointInSubpath = function(subpath, x, y) {
    var inside = false;
    var n = subpath.length;
    if (n < 2) return false;
    for (var i = 0, j = n - 1; i < n; j = i++) {
        var xi = subpath[i][0], yi = subpath[i][1];
        var xj = subpath[j][0], yj = subpath[j][1];
        // Standard ray-cast: edge crosses horizontal ray iff yi and yj
        // straddle y, and the x-intersect is to the right of x.
        var intersect = ((yi > y) !== (yj > y))
            && (x < (xj - xi) * (y - yi) / ((yj - yi) || 1e-30) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
};

CanvasRenderingContext2D.prototype.isPointInPath = function(/* path? */ x, y, fillRule) {
    // First arg may be a Path2D object — not implemented; treat as the
    // 2-arg form and shift indices.
    if (arguments.length >= 1 && typeof x === "object" && x !== null) {
        x = arguments[1];
        y = arguments[2];
        fillRule = arguments[3];
    }
    void fillRule;
    var nx = +x, ny = +y;
    if (!isFinite(nx) || !isFinite(ny)) return false;
    var subs = this._pathSubpaths;
    for (var i = 0; i < subs.length; ++i) {
        if (this._pointInSubpath(subs[i], nx, ny)) return true;
    }
    return false;
};

// isPointInStroke: hit-test against a stroke-thickened version of the
// path. Approximation: distance from the point to any path segment is
// less than half the current lineWidth. Spec accepts an optional
// Path2D object as the first arg — same caveat as isPointInPath.
CanvasRenderingContext2D.prototype.isPointInStroke = function(/* path? */ x, y) {
    if (arguments.length >= 1 && typeof x === "object" && x !== null) {
        x = arguments[1];
        y = arguments[2];
    }
    var nx = +x, ny = +y;
    if (!isFinite(nx) || !isFinite(ny)) return false;
    var halfWidth = (+this.lineWidth || 1) * 0.5;
    var subs = this._pathSubpaths;
    for (var i = 0; i < subs.length; ++i) {
        var sp = subs[i];
        for (var j = 1; j < sp.length; ++j) {
            var ax = sp[j - 1][0], ay = sp[j - 1][1];
            var bx = sp[j][0],     by = sp[j][1];
            // Closest-point-on-segment distance.
            var dx = bx - ax, dy = by - ay;
            var len2 = dx * dx + dy * dy;
            var t = (len2 > 0) ? ((nx - ax) * dx + (ny - ay) * dy) / len2 : 0;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            var px = ax + t * dx, py = ay + t * dy;
            var ex = nx - px, ey = ny - py;
            if (ex * ex + ey * ey <= halfWidth * halfWidth) return true;
        }
    }
    return false;
};

// ── pulp #964 — Text drawing ──────────────────────────────────────────────
CanvasRenderingContext2D.prototype.fillText = function(text, x, y, maxWidth) {
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._syncDirectionState();
    this._syncTextState();
    this._applyFillStyle();
    // canvasFillText takes (id, text, x, y, size, color, maxWidth). When
    // the active fillStyle is a gradient the bridge keeps the gradient
    // active on the canvas; canvasFillText still records a colour, so
    // pass the gradient's first stop as a graceful approximation.
    var color = this.fillStyle;
    if (color && color._kind) {
        color = (color._stops && color._stops.length > 0) ? color._stops[0].color : "#fff";
    }
    // pulp #1434 — parse `<size>` from the full CSS font shorthand. The
    // family/weight/slant already flowed through canvasSetFontFull during
    // _syncTextState; canvasFillText only needs the size for its own
    // baseline math.
    var parsed = CanvasRenderingContext2D._parseFontShorthand(this.font || "14px Inter");
    // pulp #1525 — Canvas2D `fillText(text, x, y, maxWidth)`. Thread the
    // optional maxWidth through to the bridge as a 7th arg in CSS px.
    // Spec: `<= 0`, NaN, Infinity, or undefined all mean "no constraint",
    // so we coerce to a finite positive number or 0 (the bridge sentinel).
    var mw = +maxWidth;
    if (!(mw > 0) || !isFinite(mw)) mw = 0;
    if (typeof canvasFillText === "function") {
        canvasFillText(this._id, String(text == null ? "" : text), x, y, parsed.size, String(color), mw);
    }
};

CanvasRenderingContext2D.prototype.strokeText = function(text, x, y, maxWidth) {
    // pulp #1525 — true outlined-glyph rendering when the host has
    // canvasStrokeText (Pulp ≥ 0.78). On older hosts that pre-date this
    // bridge entry, fall back to the pre-#1525 fillText-with-strokeColor
    // approximation so the call doesn't throw — the JS shim must still
    // produce visible text on a v0.77 host loading a v0.78 plugin.
    this._syncGlobalState();
    this._syncShadowState();
    this._syncTextState();
    // strokeText uses strokeStyle, not fillStyle. Push the active
    // strokeStyle + lineWidth through the bridge so Canvas::stroke_text
    // picks them up when it builds its stroke paint.
    this._applyStrokeStyle();
    var color = this.strokeStyle;
    if (color && color._kind) {
        // pulp Wave 3 c2d.7 — strokeStyle gradient is bridged through
        // _applyStrokeStyle's per-kind setter (canvasSetStrokeLinearGradient,
        // etc.), which populates stroke_shader_ on the Skia canvas. The
        // colour we pass to canvasStrokeText is only used as a solid
        // fallback if stroke_shader_ is null at paint time, so we still
        // pull the first stop here for backwards-compat / pattern
        // fallback. apply_stroke_state will prefer the shader when set.
        color = (color._stops && color._stops.length > 0) ? color._stops[0].color : "#fff";
    }
    var parsed = CanvasRenderingContext2D._parseFontShorthand(this.font || "14px Inter");
    var mw = +maxWidth;
    if (!(mw > 0) || !isFinite(mw)) mw = 0;
    if (typeof canvasStrokeText === "function") {
        canvasStrokeText(this._id, String(text == null ? "" : text), x, y, parsed.size, String(color), mw);
        return;
    }
    // Backwards-compat fallback: pre-#1525 hosts route stroke through
    // fillText with strokeStyle as the fill colour. Visually close
    // enough for HUD/Filterbank text on legacy builds.
    var savedFill = this.fillStyle;
    this.fillStyle = this.strokeStyle;
    try { this.fillText(text, x, y, maxWidth); }
    finally { this.fillStyle = savedFill; }
};

// ── pulp #964 — Gradient factories ────────────────────────────────────────
CanvasRenderingContext2D.prototype.createLinearGradient = function(x0, y0, x1, y1) {
    return new CanvasGradient("linear", { x0: x0, y0: y0, x1: x1, y1: y1 });
};

CanvasRenderingContext2D.prototype.createRadialGradient = function(x0, y0, r0, x1, y1, r1) {
    // pulp #1524 — true two-circle radial gradient. Carry both circles in
    // the gradient handle; the fillStyle flush picks the two-circle bridge
    // (canvasSetRadialGradientTwoCircles) when available and falls back to
    // the single-circle outer-only path on older binaries. Skia routes
    // through SkGradientShader::MakeTwoPointConical; CG routes through
    // CGContextDrawRadialGradient with both circles.
    return new CanvasGradient("radial", {
        x0: +x0 || 0, y0: +y0 || 0, r0: +r0 || 0,
        x1: +x1 || 0, y1: +y1 || 0, r1: +r1 || 0
    });
};

CanvasRenderingContext2D.prototype.createConicGradient = function(startAngle, cx, cy) {
    // pulp #1434 bridge-thin gap-fill — build a real conic CanvasGradient.
    // Spec signature: createConicGradient(startAngle, x, y) where startAngle
    // is in radians. The Skia backend renders the sweep via
    // SkGradientShader::MakeSweep; the CG backend degrades to the first
    // stop's flat colour (no native conic shader). The gradient is flushed
    // via canvasSetConicGradient when assigned to ctx.fillStyle.
    return new CanvasGradient("conic", {
        cx: +cx || 0,
        cy: +cy || 0,
        startAngle: +startAngle || 0
    });
};

CanvasRenderingContext2D.prototype.createPattern = function(image, repetition) {
    // pulp #1434 bridge-thin gap-fill — real CanvasPattern. Returns a
    // CanvasPattern handle that ctx.fillStyle / ctx.strokeStyle accept;
    // _applyFillStyle flushes via canvasSetFillPattern when a pattern is
    // the active fillStyle. Spec repetition values:
    //   "repeat" (default), "repeat-x", "repeat-y", "no-repeat"
    // Per spec, an empty / null `repetition` argument defaults to
    // "repeat"; an unrecognised value would throw SyntaxError, but we
    // softly coerce to "repeat" to keep recording plugins from crashing.
    var rep = repetition;
    if (rep == null || rep === "") rep = "repeat";
    rep = String(rep);
    if (rep !== "repeat" && rep !== "repeat-x"
        && rep !== "repeat-y" && rep !== "no-repeat") {
        rep = "repeat";
    }
    // Map spec repetition onto a (tile_x, tile_y) pair the bridge consumes.
    // Skia translates these via SkTileMode (kRepeat for repeat-on-axis,
    // kDecal for "no repeat on this axis").
    var tx, ty;
    if (rep === "repeat")     { tx = "repeat";  ty = "repeat";  }
    else if (rep === "repeat-x") { tx = "repeat";  ty = "no-repeat"; }
    else if (rep === "repeat-y") { tx = "no-repeat"; ty = "repeat";  }
    else /* no-repeat */     { tx = "no-repeat"; ty = "no-repeat"; }
    // Image source — accept either a string path / data URI, or an
    // image-like object with .src / ._src (matches drawImage normalisation).
    var src = "";
    if (typeof image === "string") src = image;
    else if (image && typeof image.src === "string") src = image.src;
    else if (image && typeof image._src === "string") src = image._src;
    // Spec: returning null is permissible when the source is unavailable.
    // We require a non-empty src to flush meaningful state to the bridge.
    if (!src) return null;
    return new CanvasPattern(src, tx, ty);
};

// ── Canvas2D API gap closures (issue-916) ────────────────────────────
// measureText returns an HTML5 TextMetrics object — width plus the
// actualBoundingBox{Left,Right,Ascent,Descent} and
// fontBoundingBox{Ascent,Descent} fields callers need for proper text
// alignment. Falls back to a zero-filled object if the bridge function
// isn't available (older host).
CanvasRenderingContext2D.prototype.measureText = function(text) {
    if (typeof canvasMeasureText !== "function") {
        // Coarse estimate — avoids returning undefined/null which would
        // break callers that destructure the result. pulp #1434 — pull
        // size out of the parsed shorthand so multi-token strings don't
        // collapse to 14 by way of `parseFloat("italic bold")`.
        var fb = CanvasRenderingContext2D._parseFontShorthand(this.font || "14px Inter");
        var px = fb.size || 14;
        var w = String(text == null ? "" : text).length * px * 0.6;
        return {
            width: w,
            actualBoundingBoxLeft: 0,
            actualBoundingBoxRight: w,
            actualBoundingBoxAscent: px * 0.75,
            actualBoundingBoxDescent: px * 0.25,
            fontBoundingBoxAscent: px * 0.75,
            fontBoundingBoxDescent: px * 0.25
        };
    }
    // pulp #1434 — share the full CSS font shorthand parser with
    // _syncTextState so multi-token strings (`'italic bold 14px Inter'`)
    // measure with the same size+family the bridge actually rendered.
    var parsed = CanvasRenderingContext2D._parseFontShorthand(this.font || "14px Inter");
    return canvasMeasureText(this._id, String(text == null ? "" : text), parsed.family, parsed.size);
};

// drawImage(img, dx, dy) / drawImage(img, dx, dy, dw, dh) /
// drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh) — only the first two
// signatures are wired through the bridge today (issue-916). The 9-arg
// source-rect form is recorded as the destination-only form and the
// source rect is currently ignored — file a follow-up if a Pulp plugin
// needs sprite-sheet slicing.
CanvasRenderingContext2D.prototype.drawImage = function(img, a, b, c, d, e, f, g, h) {
    if (typeof canvasDrawImage !== "function") return;
    // pulp #1434 — flush imageSmoothing state so Skia / CG honour the
    // current ctx.imageSmoothingEnabled + Quality on this draw.
    this._syncImageSmoothingState();
    // pulp #1520 — flush filter state so the chosen image filter
    // (blur, grayscale, …) wraps this drawImage too.
    this._syncFilterState();
    var src = "";
    if (typeof img === "string") src = img;
    else if (img && typeof img.src === "string") src = img.src;
    else if (img && typeof img._src === "string") src = img._src;
    var dx, dy, dw, dh;
    if (arguments.length <= 3) {
        // drawImage(img, dx, dy) — use intrinsic size if known.
        dx = a; dy = b;
        dw = (img && img.width)  ? img.width  : 0;
        dh = (img && img.height) ? img.height : 0;
    } else if (arguments.length <= 5) {
        // drawImage(img, dx, dy, dw, dh)
        dx = a; dy = b; dw = c; dh = d;
    } else {
        // drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh)
        // — record dst rect; source-rect slicing not yet wired.
        dx = e; dy = f; dw = g; dh = h;
    }
    canvasDrawImage(this._id, src, dx, dy, dw, dh);
};

// setLineDash([5, 3, 2, ...]) — even-length arrays are taken verbatim;
// the bridge duplicates odd-length arrays per spec. lineDashOffset is
// recorded as the dash phase via a getter/setter pair below.
CanvasRenderingContext2D.prototype.setLineDash = function(pattern) {
    if (typeof canvasSetLineDash !== "function") return;
    if (!Array.isArray(pattern)) pattern = [];
    this._lineDash = pattern.slice();
    canvasSetLineDash(this._id, pattern, this.lineDashOffset || 0);
};

CanvasRenderingContext2D.prototype.getLineDash = function() {
    // HTML5: returns a copy of the current pattern (spec disallows
    // returning the same array).
    return this._lineDash ? this._lineDash.slice() : [];
};

// getImageData(x, y, w, h) → { data: Uint8ClampedArray, width, height }
// The bridge returns a base64-encoded RGBA blob; we decode it to a
// Uint8ClampedArray so consumers see the standard layout. Returns a
// zero-filled buffer if the bridge isn't available or the canvas
// hasn't been rasterized yet (RecordingCanvas / not-yet-painted).
CanvasRenderingContext2D.prototype.getImageData = function(x, y, w, h) {
    var width  = w | 0;
    var height = h | 0;
    var byteCount = width * height * 4;
    var buf = (typeof Uint8ClampedArray !== "undefined")
        ? new Uint8ClampedArray(byteCount)
        : new Array(byteCount);
    if (typeof canvasGetImageData === "function") {
        var raw = canvasGetImageData(this._id, x | 0, y | 0, width, height);
        if (raw && raw.data && typeof raw.data === "string") {
            // base64 → bytes — minimal decoder, ignores whitespace.
            var alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            var lookup = {};
            for (var i = 0; i < alphabet.length; ++i) lookup[alphabet.charAt(i)] = i;
            var s = raw.data, oi = 0;
            for (var bi = 0; bi < s.length; bi += 4) {
                var a = lookup[s.charAt(bi)]     || 0;
                var bb = lookup[s.charAt(bi+1)]  || 0;
                var cc = (s.charAt(bi+2) === "=") ? 0 : (lookup[s.charAt(bi+2)] || 0);
                var dd = (s.charAt(bi+3) === "=") ? 0 : (lookup[s.charAt(bi+3)] || 0);
                if (oi < byteCount) buf[oi++] = (a << 2) | (bb >> 4);
                if (s.charAt(bi+2) !== "=" && oi < byteCount) buf[oi++] = ((bb & 0xF) << 4) | (cc >> 2);
                if (s.charAt(bi+3) !== "=" && oi < byteCount) buf[oi++] = ((cc & 0x3) << 6) | dd;
            }
        }
    }
    return { data: buf, width: width, height: height };
};

// putImageData(imageData, dx, dy) — encodes the Uint8ClampedArray as
// base64 and hands it to the bridge for rasterization on the next
// paint. Source-rect form (putImageData(img, dx, dy, dirtyX, dirtyY,
// dirtyW, dirtyH)) is treated as the no-rect form for now — file a
// follow-up if sub-rect updates become a hot path.
CanvasRenderingContext2D.prototype.putImageData = function(imageData, dx, dy) {
    if (!imageData || typeof canvasPutImageData !== "function") return;
    var data = imageData.data || [];
    var width  = imageData.width  | 0;
    var height = imageData.height | 0;
    if (width <= 0 || height <= 0) return;
    var alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    var n = width * height * 4;
    var out = "";
    for (var i = 0; i < n; i += 3) {
        var a = data[i]     | 0;
        var bb = (i+1 < n) ? (data[i+1] | 0) : 0;
        var cc = (i+2 < n) ? (data[i+2] | 0) : 0;
        var nbits = (a << 16) | (bb << 8) | cc;
        out += alphabet.charAt((nbits >> 18) & 0x3F);
        out += alphabet.charAt((nbits >> 12) & 0x3F);
        out += (i+1 < n) ? alphabet.charAt((nbits >> 6) & 0x3F) : "=";
        out += (i+2 < n) ? alphabet.charAt(nbits & 0x3F) : "=";
    }
    canvasPutImageData(this._id, out, width, height, dx | 0, dy | 0);
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
        var fragment = currentPipeline.fragment || {};
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
            context._configured = !!nativeState.configured;
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
