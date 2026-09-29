// ═══════════════════════════════════════════════════════════════════════════════
// HTMLCanvasElement + CanvasRenderingContext2D
// ═══════════════════════════════════════════════════════════════════════════════

// CanvasGradient is returned by createLinearGradient / createRadialGradient /
// createConicGradient and can be assigned to ctx.fillStyle / ctx.strokeStyle.
// Stops accumulate via addColorStop and flush to the bridge when the gradient
// is active.
function CanvasGradient(kind, params) {
    this._kind = kind;             // "linear" | "radial" | "conic"
    this._params = params || {};
    this._stops = [];              // [{ offset, color }, ...]
}
CanvasGradient.prototype.addColorStop = function(offset, color) {
    this._stops.push({ offset: Number(offset) || 0, color: String(color || "") });
};

// CanvasPattern is returned by ctx.createPattern(image, repetition) and can
// be assigned to ctx.fillStyle / ctx.strokeStyle. Repetition values per
// Canvas2D spec:
//   "repeat" (default), "repeat-x", "repeat-y", "no-repeat"
// Image source is reduced to a path / data URL string the same way
// drawImage normalizes it. Flushed to the bridge via canvasSetFillPattern
// when assigned to ctx.fillStyle. The Skia backend renders the real
// tiled pattern via SkShader::MakeImage with SkTileMode::{kRepeat,kDecal};
// the CG backend renders fill patterns through CGPattern callbacks. Stroke
// patterns can still fall back to the active stroke color on backends
// without stroke-pattern support.
function CanvasPattern(src, tileX, tileY) {
    this._kind = "pattern";
    this._src = String(src || "");
    this._tileX = String(tileX || "repeat");
    this._tileY = String(tileY || "repeat");
}

function __pulpCanvasPositiveFiniteOrZero(value) {
    if (typeof value === "number") {
        return (value > 0 && value !== Infinity) ? value : 0;
    }
    if (typeof value === "string") {
        var s = value.trim();
        if (/^[+-]?(?:(?:\d+\.?\d*)|(?:\.\d+))(?:[eE][+-]?\d+)?$/.test(s)) {
            var n = parseFloat(s);
            return (n > 0 && n !== Infinity) ? n : 0;
        }
    }
    if (value === true) return 1;
    return 0;
}

function CanvasRenderingContext2D(canvasEl) {
    this.canvas = canvasEl;
    this._id = canvasEl._id;
    this.fillStyle = "#000000";
    this.strokeStyle = "#000000";
    this.lineWidth = 1;
    this.font = "14px Inter";
    // Canvas2D state values that the bridge accepts through dedicated
    // canvas* functions. Plain JS fields preserve getter round-trips; the
    // _sync* helpers below push them on demand.
    this.textAlign = "left";
    // Canvas2D's initial textBaseline is "alphabetic" — the y handed to
    // fillText is the baseline itself. Defaulting to "top" instead moved
    // every caption that never assigns textBaseline down by one ascent,
    // so text authored just above a rect landed inside it.
    this.textBaseline = "alphabetic";
    this.lineCap = "butt";
    this.lineJoin = "miter";
    this.miterLimit = 10;
    // lineDashOffset re-flushes the active dash pattern on assignment so
    // mutating between draws shifts the phase immediately. Backed by
    // `_lineDashOffset`; exposed via the concatenated prelude's
    // defineProperty pair.
    this._lineDashOffset = 0;
    this.globalAlpha = 1;
    this.globalCompositeOperation = "source-over";
    this.imageSmoothingEnabled = true;
    this.imageSmoothingQuality = "low";
    // Canvas2D ctx.direction. Spec values: "ltr" | "rtl" | "inherit"
    // (we treat "inherit" as the default ltr, matching the spec's
    // "directionality from the canvas element / document" resolution path
    // on a host that doesn't expose a writing-direction computed style yet).
    // Tracked locally so the getter round-trips and flushed to the bridge
    // by `_syncDirectionState` before the next fillText. strokeText does
    // not currently flush direction state.
    this.direction = "ltr";
    // Canvas2D ctx.filter. Spec: a CSS <filter-function-list> string
    // applied to subsequent draw operations (blur, brightness, contrast,
    // drop-shadow, grayscale, hue-rotate, invert, opacity, saturate, sepia).
    // The default is "none". Tracked locally and flushed to the bridge by
    // `_syncFilterState` before fill/stroke paths, rects, fillText, and
    // drawImage. strokeText does not currently flush filter state.
    this.filter = "none";
    // Canvas2D drop-shadow state. Each property mirrors the spec defaults:
    // shadow inactive (transparent black, zero blur, zero offset). Tracked
    // locally so getters round-trip, then flushed by `_syncShadowState`
    // before fill, stroke, and text draws.
    this.shadowColor = "rgba(0, 0, 0, 0)";
    this.shadowBlur = 0;
    this.shadowOffsetX = 0;
    this.shadowOffsetY = 0;
    this._lineDash = [];
    // _activeFillKind tracks whether the most recently applied fillStyle was
    // a "color" or a "gradient". When a gradient is active the next
    // canvasFillRect / canvasFillPath uses the bridge's active gradient
    // state; the shim does NOT call canvasSetFillColor before the draw or
    // the gradient would be overwritten back to a solid color.
    this._activeFillKind = "color";
    this._activeStrokeKind = "color";
    // Cache of last-pushed font / textAlign / textBaseline / line* / global*
    // state so we don't spam the bridge with redundant set_* commands.
    this._sentFont = null;
    this._sentTextAlign = null;
    this._sentTextBaseline = null;
    this._sentLineCap = null;
    this._sentLineJoin = null;
    // Track miterLimit and imageSmoothing* sticky state so
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
    // Sticky direction / filter state caches.
    this._sentDirection = null;
    this._sentFilter = null;
    // Last solid fill colour, stroke colour and line width sent. A draw
    // re-sends them only when they differ from what the native canvas holds.
    this._sentFillColor = null;
    this._sentStrokeColor = null;
    this._sentLineWidth = null;
    // JS-side mirror of the current 2D affine transform, tracked by
    // translate / scale / rotate / setTransform / transform and the save /
    // restore stack. The bridge replays draw commands at paint() time, so
    // the C++ canvas does not have a "current matrix" queryable
    // synchronously from JS. We mirror it here so getTransform can return a
    // DOMMatrix-shaped object without a round-trip. Layout:
    //   [a, b, c, d, e, f]  (matches HTML5 spec / DOMMatrix2DInit)
    //     | a c e |
    //     | b d f |
    //     | 0 0 1 |
    this._currentTransform = [1, 0, 0, 1, 0, 0];
    // Drawing-state snapshots pushed by save() and popped by restore().
    this._stateStack = [];
    // Number of active clip intersections in the current save/restore state.
    // A full-canvas clear can replace the retained native command stream only
    // when no persistent clip is active; otherwise clearRect is intentionally
    // restricted and older pixels may still contribute outside the clip.
    this._clipDepth = 0;
    // Canvas2D normally preserves every command until the backing store is
    // resized. Materialized browser applications opt into a retained-frame
    // subset where a provable full-backing-store clear begins a replacement
    // frame. Keeping this opt-in avoids changing ordinary Canvas semantics.
    this._pulpRetainedCanvasFrames = globalThis.__pulpRetainedCanvasFrames__ === true;
    // A browser canvas records in backing-store pixels, then independently
    // scales that backing store into its CSS layout box. CanvasWidget replays
    // directly into that CSS-sized logical box. Materialized browser
    // applications therefore divide each output axis by the ACTUAL
    // backing-store-to-CSS ratio, not merely by devicePixelRatio. Those are
    // equal for an unscaled canvas, but differ whenever a fixed authored UI is
    // proportionally fitted (for example 1320x860 authored into 1228x800).
    // Read the ratio lazily in _setBridgeTransform because responsive layout
    // and canvas width/height assignments can both change it after getContext.
    this._pulpLogicalCanvasScale = globalThis.__pulpLogicalCanvasScale__ === true;
    // JS-side mirror of the current path so isPointInPath / isPointInStroke
    // can answer synchronously via a JS hit test. Each subpath is a flat
    // [x0, y0, x1, y1, ...] number array appended by moveTo / lineTo, so a
    // lineTo costs two pushes rather than a point allocation; cubic and
    // quadratic curves are sampled into straight-edge approximations. Arc, arcTo,
    // ellipse, and roundRect are bridge-only today and are not mirrored for
    // synchronous hit tests. The bridge owns the canonical SkPath used for
    // fill / stroke / clip; this JS mirror exists only for query methods.
    this._pathSubpaths = [];
    // True while pulpCachedGroup is recording drawFn into a native group.
    this._groupRecording = false;
    // Save-stack depth a restore() may not pop below (0: unbounded).
    this._stateFloor = 0;
    // A new context is a new script realm drawing this canvas; groups another
    // realm recorded are not its content.
    if (typeof canvasInvalidateGroup === "function") canvasInvalidateGroup(this._id);
}

// Every `_sent*` cache field. save() snapshots them, restore() puts them back,
// and a native command-stream replacement clears them.
CanvasRenderingContext2D._SENT_FIELDS = [
    "_sentFont", "_sentTextAlign", "_sentTextBaseline",
    "_sentLineCap", "_sentLineJoin", "_sentMiterLimit",
    "_sentImageSmoothingEnabled", "_sentImageSmoothingQuality",
    "_sentGlobalAlpha", "_sentGlobalCompositeOperation",
    "_sentShadowColor", "_sentShadowBlur", "_sentShadowOffsetX", "_sentShadowOffsetY",
    "_sentDirection", "_sentFilter",
    "_sentFillColor", "_sentStrokeColor", "_sentLineWidth"
];
// The Canvas2D drawing state that save()/restore() covers (the current path
// and the transform are handled separately).
CanvasRenderingContext2D._STATE_FIELDS = [
    "fillStyle", "strokeStyle", "lineWidth", "lineCap", "lineJoin", "miterLimit",
    "_lineDash", "_lineDashOffset", "font", "textAlign", "textBaseline",
    "direction", "globalAlpha", "globalCompositeOperation",
    "imageSmoothingEnabled", "imageSmoothingQuality", "filter",
    "shadowColor", "shadowBlur", "shadowOffsetX", "shadowOffsetY",
    "_activeFillKind", "_activeStrokeKind"
];
CanvasRenderingContext2D.prototype._clearSentState = function() {
    var f = CanvasRenderingContext2D._SENT_FIELDS;
    for (var i = 0; i < f.length; ++i) this[f[i]] = null;
};

// Send a solid colour / line width only when the native canvas does not
// already hold it.
CanvasRenderingContext2D.prototype._sendFillColor = function(color) {
    this._fp();
    if (this._sentFillColor === color) return;
    if (typeof canvasSetFillColor === "function") canvasSetFillColor(this._id, color);
    this._sentFillColor = color;
};
CanvasRenderingContext2D.prototype._sendStrokeColor = function(color) {
    this._fp();
    if (this._sentStrokeColor === color) return;
    if (typeof canvasSetStrokeColor === "function") canvasSetStrokeColor(this._id, color);
    this._sentStrokeColor = color;
};
CanvasRenderingContext2D.prototype._syncLineWidth = function() {
    this._fp();
    var w = this.lineWidth;
    if (this._sentLineWidth === w) return;
    if (typeof canvasSetLineWidth === "function") canvasSetLineWidth(this._id, w);
    this._sentLineWidth = w;
};

CanvasRenderingContext2D.prototype._setBridgeTransform = function(a, b, c, d, e, f) {
    this._fp();
    if (typeof canvasSetTransform !== "function") return;
    var sx = 1, sy = 1;
    if (this._pulpLogicalCanvasScale) {
        var cssWidth = Number(this.canvas && this.canvas.clientWidth);
        var cssHeight = Number(this.canvas && this.canvas.clientHeight);
        var backingWidth = Number(this.canvas && this.canvas.width);
        var backingHeight = Number(this.canvas && this.canvas.height);
        sx = (cssWidth > 0 && isFinite(cssWidth) &&
              backingWidth > 0 && isFinite(backingWidth))
            ? backingWidth / cssWidth : 0;
        sy = (cssHeight > 0 && isFinite(cssHeight) &&
              backingHeight > 0 && isFinite(backingHeight))
            ? backingHeight / cssHeight : 0;
        // During initial detached layout clientWidth/clientHeight may still be
        // zero. Preserve the former DPR behavior as the conservative fallback
        // until the element has a real native layout box.
        var dpr = Number((globalThis.window && globalThis.window.devicePixelRatio)
                         || globalThis.devicePixelRatio || 1);
        if (!(dpr > 0) || !isFinite(dpr)) dpr = 1;
        if (!(sx > 0) || !isFinite(sx)) sx = dpr;
        if (!(sy > 0) || !isFinite(sy)) sy = dpr;
    }
    // For |a c e| / |b d f|, CSS backing-store presentation scales the
    // complete x output row by 1/sx and y output row by 1/sy.
    canvasSetTransform(this._id,
        a / sx, b / sy, c / sx, d / sy, e / sx, f / sy);
};

// DOMMatrix-like return value for getTransform(). The HTML5 spec returns a
// `DOMMatrix` instance with `a, b, c, d, e, f` and the `is2D` / `isIdentity`
// flags. The shim supports the 2D affine mutators, multiplication, inverse,
// JSON, and typed-array readers used by plugin code. 3D axis operations and
// decomposition remain outside the Canvas bridge layer.

CanvasRenderingContext2D.prototype._applyFillStyle = function() {
    this._fp();
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
        // Prefer the two-circle bridge. Both Skia
        // (MakeTwoPointConical) and CG (CGContextDrawRadialGradient with
        // both circles) honor the inner circle. Older binaries without
        // that bridge fall through to the single-circle outer-only path so
        // JS hot-reload doesn't crash.
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
    // ctx.createConicGradient routes through SkGradientShader::MakeSweep on
    // Skia; CoreGraphics software-rasterizes a cached conic image. Same
    // flush shape as linear/radial.
    if (fs && fs._kind === "conic" && typeof canvasSetConicGradient === "function") {
        var pc = fs._params, sc = fs._stops;
        var ac = [this._id, pc.cx, pc.cy, pc.startAngle];
        for (var k = 0; k < sc.length; ++k) { ac.push(sc[k].color); ac.push(sc[k].offset); }
        canvasSetConicGradient.apply(null, ac);
        this._activeFillKind = "gradient";
        return;
    }
    // ctx.createPattern uses real tiled paint via SkShader::MakeImage with
    // SkTileMode per axis on Skia. CG fills use a CGPattern tile callback;
    // stroke patterns remain backend-dependent and can fall back to color.
    // We reuse `_activeFillKind = "gradient"` as the "non-color" sentinel
    // so canvasClearGradient resets correctly when the next fillStyle
    // assignment is a plain string.
    if (fs && fs._kind === "pattern" && typeof canvasSetFillPattern === "function") {
        canvasSetFillPattern(this._id, fs._src, fs._tileX, fs._tileY);
        this._activeFillKind = "gradient";
        return;
    }
    // Solid color. Clear any active gradient so subsequent fills don't pick
    // up a stale gradient (Canvas2D spec: assigning fillStyle replaces the
    // previous style outright).
    if (this._activeFillKind === "gradient" && typeof canvasClearGradient === "function") {
        canvasClearGradient(this._id);
    }
    this._activeFillKind = "color";
    this._sendFillColor(String(fs == null ? "" : fs));
};

CanvasRenderingContext2D.prototype._applyStrokeStyle = function() {
    this._fp();
    var ss = this.strokeStyle;
    // CanvasPattern as strokeStyle. If the bridge exposes
    // canvasSetStrokePattern, flush the pattern; otherwise fall through to
    // the solid-fallback below.
    if (ss && ss._kind === "pattern" && typeof canvasSetStrokePattern === "function") {
        canvasSetStrokePattern(this._id, ss._src, ss._tileX, ss._tileY);
        this._syncLineWidth();
        this._activeStrokeKind = "pattern";
        return;
    }
    // Gradient strokeStyle mirrors _applyFillStyle: prefer the per-kind
    // stroke-gradient bridge function when present; fall back to the
    // first-stop solid color for binaries without that bridge so JS
    // hot-reload doesn't crash.
    if (ss && ss._kind === "linear" && typeof canvasSetStrokeLinearGradient === "function") {
        var lp = ss._params, ls = ss._stops;
        var largs = [this._id, lp.x0, lp.y0, lp.x1, lp.y1];
        for (var li = 0; li < ls.length; ++li) { largs.push(ls[li].color); largs.push(ls[li].offset); }
        canvasSetStrokeLinearGradient.apply(null, largs);
        this._syncLineWidth();
        this._activeStrokeKind = "gradient";
        return;
    }
    if (ss && ss._kind === "radial") {
        var rp = ss._params, rs = ss._stops;
        if (typeof canvasSetStrokeRadialGradientTwoCircles === "function") {
            var rargs = [this._id, rp.x0, rp.y0, rp.r0, rp.x1, rp.y1, rp.r1];
            for (var ri = 0; ri < rs.length; ++ri) { rargs.push(rs[ri].color); rargs.push(rs[ri].offset); }
            canvasSetStrokeRadialGradientTwoCircles.apply(null, rargs);
            this._syncLineWidth();
            this._activeStrokeKind = "gradient";
            return;
        }
        if (typeof canvasSetStrokeRadialGradient === "function") {
            var sargs = [this._id, rp.x1, rp.y1, rp.r1];
            for (var si = 0; si < rs.length; ++si) { sargs.push(rs[si].color); sargs.push(rs[si].offset); }
            canvasSetStrokeRadialGradient.apply(null, sargs);
            this._syncLineWidth();
            this._activeStrokeKind = "gradient";
            return;
        }
    }
    if (ss && ss._kind === "conic" && typeof canvasSetStrokeConicGradient === "function") {
        var cp = ss._params, cs = ss._stops;
        var cargs = [this._id, cp.cx, cp.cy, cp.startAngle];
        for (var ci = 0; ci < cs.length; ++ci) { cargs.push(cs[ci].color); cargs.push(cs[ci].offset); }
        canvasSetStrokeConicGradient.apply(null, cargs);
        this._syncLineWidth();
        this._activeStrokeKind = "gradient";
        return;
    }
    // Fallback: gradient bridge fn missing (older binary), or
    // CanvasPattern with no stroke-pattern bridge. Pull a solid
    // color — first stop for gradients, neutral grey for unsupported
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
        // Solid-color assignment after a gradient or pattern: clear the
        // stroke shader both install, so the next stroke uses
        // set_stroke_color cleanly (mirrors fillStyle's canvasClearGradient
        // flush).
        if ((this._activeStrokeKind === "gradient" || this._activeStrokeKind === "pattern") &&
            typeof canvasClearStrokeGradient === "function") {
            canvasClearStrokeGradient(this._id);
        }
        this._activeStrokeKind = "color";
    }
    this._sendStrokeColor(colorStr);
    this._syncLineWidth();
};

// Parse the CSS Fonts Module Level 4 `font` shorthand:
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
// Unknown tokens are silently dropped — matches browser behavior where
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
    // Convert non-px units to px so values like `1.2em Inter`,
    // `12pt Inter`, `1rem Inter` produce sane sizes instead of being
    // treated as `1.2px / 12px / 1px`. Canvas2D has no DOM cascade, so
    // em/rem resolve against a fixed 16px root — same default browsers use
    // at the document root and what headless Canvas2D shims use.
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

// Push state-setter values to the bridge before any draw that depends on
// them. Cheap (only sends what changed) and idempotent.
CanvasRenderingContext2D.prototype._syncTextState = function() {
    this._fp();
    if (this._sentFont !== this.font) {
        var parsed = CanvasRenderingContext2D._parseFontShorthand(
            this.font || "14px Inter");
        // Stash the parsed line-height + variant for measureText round-tripping
        // and for any future bridge plumbing (CSS line-height is currently a
        // shim-side concern; variant has no canvas-API surface yet).
        this._parsedLineHeight = parsed.lineHeight;
        this._parsedFontVariant = parsed.variant;
        // Prefer the rich bridge fn when the host registered it (canvas
        // widgets only — see widget_bridge/canvas2d_api.cpp). Falls back to
        // the legacy canvasSetFont(id, family, size) on older hosts.
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
    this._fp();
    if (this._sentLineCap !== this.lineCap) {
        if (typeof canvasSetLineCap === "function") canvasSetLineCap(this._id, this.lineCap);
        this._sentLineCap = this.lineCap;
    }
    if (this._sentLineJoin !== this.lineJoin) {
        if (typeof canvasSetLineJoin === "function") canvasSetLineJoin(this._id, this.lineJoin);
        this._sentLineJoin = this.lineJoin;
    }
    // Push ctx.miterLimit to the bridge so SkPaint::setStrokeMiter /
    // CGContextSetMiterLimit honor the JS value. Spec: ignore non-finite
    // / non-positive.
    var ml = +this.miterLimit;
    if (isFinite(ml) && ml > 0 && this._sentMiterLimit !== ml) {
        if (typeof canvasSetMiterLimit === "function") canvasSetMiterLimit(this._id, ml);
        this._sentMiterLimit = ml;
    }
};

// Flush ctx.imageSmoothingEnabled and ctx.imageSmoothingQuality before the
// next drawImage. Sticky on the C++ side; we only push when either field
// changes.
CanvasRenderingContext2D.prototype._syncImageSmoothingState = function() {
    this._fp();
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
    this._fp();
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

// Flush Canvas2D shadow state to the bridge before fill/stroke/text draws.
// Sticky on the C++ side, so we only push changed values. HTML5 spec:
// assigning a non-finite number must be silently ignored; numeric coercion
// (`+x` for any value) returns NaN for non-numerics which we treat as
// "no change" so getter round-trip still reflects the latest valid value.
CanvasRenderingContext2D.prototype._syncShadowState = function() {
    this._fp();
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

// Flush ctx.direction to the bridge. Spec values:
//   "ltr"     → 0 (default; matches SkShaper leftToRight=true)
//   "rtl"     → 1 (SkShaper leftToRight=false; HarfBuzz buffer dir RTL)
//   "inherit" → 2 (treated as 0 on backends without a per-View writing
//                  direction; the Skia backend leaves the default ltr
//                  in place, so visually identical to "ltr" for now)
// Unknown strings coerce to "ltr" silently — same shape as
// imageSmoothingQuality's defensive coercion.
CanvasRenderingContext2D.prototype._syncDirectionState = function() {
    this._fp();
    var d = String(this.direction || "ltr");
    if (d !== "ltr" && d !== "rtl" && d !== "inherit") d = "ltr";
    if (this._sentDirection === d) return;
    if (typeof canvasSetDirection === "function") {
        var enumVal = (d === "rtl") ? 1 : (d === "inherit") ? 2 : 0;
        canvasSetDirection(this._id, enumVal);
    }
    this._sentDirection = d;
};

// Flush ctx.filter to the bridge. The spec accepts a
// <filter-function-list> string ("blur(5px) sepia(80%) ...") plus the
// keyword "none". The bridge stashes the raw string; the Skia backend
// parses it into an SkImageFilter chain (blur, grayscale, sepia,
// brightness, contrast, invert, opacity, saturate, hue-rotate) and
// applies via SkPaint::setImageFilter on subsequent draws. Backends
// that don't recognize a particular function silently degrade.
//
// Unlike the CSS `filter` property on a View, this filter is per-2D-context
// state and stacks with save() / restore().
CanvasRenderingContext2D.prototype._syncFilterState = function() {
    this._fp();
    var f = String(this.filter == null ? "none" : this.filter);
    if (this._sentFilter === f) return;
    if (typeof canvasSetFilter === "function") {
        canvasSetFilter(this._id, f);
    }
    this._sentFilter = f;
};

CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {
    this._fp();
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._applyFillStyle();
    // JS standardizes on canvasRect for fillRect. The bridge also aliases
    // canvasFillRect to the same active-style handler for compatibility. The
    // 5-arg form (no color) honours the active fillStyle / gradient on the
    // C++ side.
    if (typeof canvasRect === "function") canvasRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.strokeRect = function(x, y, w, h) {
    this._fp();
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._syncLineState();
    this._applyStrokeStyle();
    if (typeof canvasStrokeRect === "function") {
        // An empty colour keeps the active strokeStyle; the native stroke_rect
        // sets the line width it carries, so pass the current one.
        var lw = +this.lineWidth;
        canvasStrokeRect(this._id, x, y, w, h, "", lw);
        this._sentLineWidth = (lw === this.lineWidth) ? lw : null;
    }
};

CanvasRenderingContext2D.prototype.clearRect = function(x, y, w, h) {
    this._fp();
    // A conventional animation frame starts with a full-backing-store clear.
    // Pulp retains Canvas2D commands for native replay, so blindly appending
    // that frame forever makes an RAF canvas grow without bound. Compact only
    // when the clear provably covers the whole backing store and no clip is
    // active. Partial/transformed/clipped clears retain normal Canvas2D
    // semantics and remain explicit clear_rect commands.
    var t = this._currentTransform || [1, 0, 0, 1, 0, 0];
    var axisAligned = t[1] === 0 && t[2] === 0 && t[0] > 0 && t[3] > 0;
    var nx = +x, ny = +y, nw = +w, nh = +h;
    var left = t[0] * nx + t[4];
    var top = t[3] * ny + t[5];
    var right = t[0] * (nx + nw) + t[4];
    var bottom = t[3] * (ny + nh) + t[5];
    // Layout may derive a fractional CSS extent from a whole-pixel backing
    // store (for example 1227.9375 CSS px -> 2456 backing px at 2x). Treat a
    // sub-pixel rounding remainder as full coverage; requiring bit-exact
    // equality rejects every later RAF and grows the retained command stream
    // forever. The tolerance is strictly less than one backing-store pixel,
    // so a genuinely partial clear remains observable.
    var coverageTolerance = 0.5;
    // A clear recorded into a cached group is part of that group, never a
    // replacement of the frame the group is being recorded into.
    var full = this._pulpRetainedCanvasFrames && !this._groupRecording
        && axisAligned && this._clipDepth === 0
        && isFinite(left) && isFinite(top) && isFinite(right) && isFinite(bottom)
        && left <= coverageTolerance && top <= coverageTolerance
        && right >= Number(this.canvas.width || 0) - coverageTolerance
        && bottom >= Number(this.canvas.height || 0) - coverageTolerance;
    if (full && typeof canvasClear === "function") {
        canvasClear(this._id);
        // Native replay starts from default state after command replacement.
        // Re-seed the current transform and invalidate lazy state caches so
        // subsequent draws faithfully reconstruct the live Canvas2D state.
        this._setBridgeTransform(t[0], t[1], t[2], t[3], t[4], t[5]);
        this._clearSentState();
        // Saved snapshots describe native state that no longer exists either.
        for (var si = 0; si < this._stateStack.length; ++si) this._stateStack[si].sent = null;
        if (typeof canvasSetLineDash === "function") {
            canvasSetLineDash(this._id, this._lineDash || [], this.lineDashOffset || 0);
        }
        this._pathSubpaths = [];
        return;
    }
    if (typeof canvasClearRect === "function") canvasClearRect(this._id, x, y, w, h);
};

CanvasRenderingContext2D.prototype.beginPath = function() {
    this._fp();
    if (typeof canvasBeginPath === "function") canvasBeginPath(this._id);
    // Reset the JS-side path mirror so isPointInPath only sees the new
    // path's geometry. Bridge canvasBeginPath does the same on the C++ side.
    this._pathSubpaths = [];
};

// A moveTo followed by a run of lineTo calls is by far the most common shape a
// script draws, and each call was its own JS->native crossing. `moveTo` opens a
// pending run instead of emitting, `lineTo` appends to it, and `_fp()` ships the
// whole run as one `canvasPathPolyline` call. A later `moveTo` (or `rect`) does
// not flush: it records where its subpath starts and keeps appending, so a
// path of many disjoint segments (tick marks, grid lines) still costs one call.
//
// The buffer is only ever a deferral, never a reordering: every other method
// that emits a bridge command calls `_fp()` first, so the command sequence the
// C++ side receives is byte-identical to the unbatched one. `check_canvas_path_flush.py`
// enforces that rule mechanically rather than by review, because a single
// missed flush would silently interleave a path into the wrong paint state --
// a corrupted frame that no pixel test would attribute back to this change.
//
// Only a run this shim opened is coalesced. A `lineTo` with no pending run
// (spec: it then behaves as `moveTo`, and the C++ path may already be open from
// an arc) takes the original one-call path unchanged.
//
// The bridge rejects a batch above 65536 coordinates as a whole, so a run is
// flushed before it would cross that.
CanvasRenderingContext2D._MAX_PENDING_COORDS = 65536;

CanvasRenderingContext2D.prototype._fp = function() {
    const pts = this._pendPts;
    if (!pts || pts.length < 2) return;
    const starts = this._pendStarts;
    this._pendPts = null;
    this._pendStarts = null;
    if (!starts && pts.length === 2) {
        // A lone moveTo: nothing to batch, and sending a 1-point polyline
        // would cost an array allocation to save nothing.
        if (typeof canvasMoveTo === "function") canvasMoveTo(this._id, pts[0], pts[1]);
        return;
    }
    if (typeof canvasPathPolyline === "function") {
        if (starts) canvasPathPolyline(this._id, pts, starts);
        else canvasPathPolyline(this._id, pts);
        return;
    }
    // Host without the batched entry point: emit the same commands one at a
    // time so an older runtime still renders correctly.
    let next = 0;
    for (let i = 0; i + 1 < pts.length; i += 2) {
        const point = i >> 1;
        const opens = point === 0 || (starts && next < starts.length && starts[next] === point);
        if (opens) {
            if (point !== 0) ++next;
            if (typeof canvasMoveTo === "function") canvasMoveTo(this._id, pts[i], pts[i + 1]);
        } else if (typeof canvasLineTo === "function") {
            canvasLineTo(this._id, pts[i], pts[i + 1]);
        }
    }
};

// Open a new subpath in the pending run, flushing first when it is full.
// `coords` is the subpath's flat point list.
CanvasRenderingContext2D.prototype._openPendingSubpath = function(coords) {
    let pts = this._pendPts;
    if (pts && pts.length + coords.length > CanvasRenderingContext2D._MAX_PENDING_COORDS) {
        this._fp();
        pts = null;
    }
    if (!pts) {
        this._pendPts = coords;
        return;
    }
    (this._pendStarts || (this._pendStarts = [])).push(pts.length >> 1);
    for (let i = 0; i < coords.length; ++i) pts.push(coords[i]);
};

CanvasRenderingContext2D.prototype.moveTo = function(x, y) {
    const nx = +x, ny = +y;
    this._openPendingSubpath([nx, ny]);
    // Open a new subpath on the JS mirror. moveTo always starts a fresh
    // subpath per the HTML5 path-construction spec.
    this._pathSubpaths.push([nx, ny]);
};

CanvasRenderingContext2D.prototype.lineTo = function(x, y) {
    const pts = this._pendPts;
    if (pts && pts.length < CanvasRenderingContext2D._MAX_PENDING_COORDS) {
        pts.push(+x, +y);
    } else {
        if (pts) this._fp();
        if (typeof canvasLineTo === "function") canvasLineTo(this._id, x, y);
    }
    this._pathMirrorLineTo(x, y);
};

CanvasRenderingContext2D.prototype.closePath = function() {
    this._fp();
    if (typeof canvasClosePath === "function") canvasClosePath(this._id);
    // Append the first point to close the loop. Spec: a closed subpath
    // behaves like an additional segment back to the start, which
    // point-in-polygon hit tests handle when the polygon is simple.
    var subs = this._pathSubpaths;
    if (subs.length > 0) {
        var last = subs[subs.length - 1];
        if (last.length >= 2) last.push(last[0], last[1]);
    }
};

CanvasRenderingContext2D.prototype.fill = function(fillRule) {
    this._fp();
    // fillRule ('evenodd' | 'nonzero') threads through to the bridge.
    // Default 'nonzero' = 0; 'evenodd' = 1.
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._applyFillStyle();
    var rule = (fillRule === "evenodd") ? 1 : 0;
    if (typeof canvasFillPath === "function") canvasFillPath(this._id, rule);
};

CanvasRenderingContext2D.prototype.stroke = function() {
    this._fp();
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._syncLineState();
    this._applyStrokeStyle();
    if (typeof canvasStrokePath === "function") canvasStrokePath(this._id);
};

// Send state assigned since the last draw before a save(), so the native
// snapshot holds it. Otherwise a value assigned ahead of a
// save/draw/restore loop is sent inside every iteration and reverted by every
// restore(). Only cached state is flushed: unchanged values cost nothing, and
// gradient and pattern styles, which are never cached, stay lazy.
CanvasRenderingContext2D.prototype._flushCachedState = function() {
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._syncLineState();
    this._syncLineWidth();
    this._syncDirectionState();
    this._syncTextState();
    var fs = this.fillStyle;
    if (!(fs && fs._kind)) this._applyFillStyle();
    var ss = this.strokeStyle;
    if (!(ss && ss._kind)) this._applyStrokeStyle();
};

// ── Canvas2D state-stack methods (save/restore) ───────────────────────────
// FilterBank and most non-trivial Canvas2D code uses save()/restore() to
// scope transforms and clip regions per draw subroutine. Without these
// shims, ctx.save() is undefined and the very first call throws TypeError,
// aborting the entire frame render. Once aborted, none of the subsequent
// drawing commands record to the bridge.
CanvasRenderingContext2D.prototype.save = function() {
    this._fp();
    this._flushCachedState();
    if (typeof canvasSave === "function") canvasSave(this._id);
    this._stateStack.push(this._captureState());
};

CanvasRenderingContext2D.prototype.restore = function() {
    this._fp();
    // Inside a cached group, a restore() cannot pop state saved outside it.
    if (this._stateFloor > 0 && this._stateStack.length <= this._stateFloor) return;
    if (typeof canvasRestore === "function") canvasRestore(this._id);
    // Spec: restoring with no matching save is a no-op.
    if (this._stateStack.length === 0) return;
    this._applyState(this._stateStack.pop());
};

// Snapshot the drawing state, the record of what the native canvas holds,
// and the JS-side transform / path / clip mirrors. The native replay reverts
// its own drawing state on restore() on every backend, so restoring can put
// the `_sent*` record back instead of discarding it and re-sending every
// setter before the next draw.
CanvasRenderingContext2D.prototype._captureState = function() {
    var state = {}, sent = {};
    var sf = CanvasRenderingContext2D._STATE_FIELDS;
    for (var i = 0; i < sf.length; ++i) state[sf[i]] = this[sf[i]];
    var cf = CanvasRenderingContext2D._SENT_FIELDS;
    for (var k = 0; k < cf.length; ++k) sent[cf[k]] = this[cf[k]];
    // Later path appends only ever extend the last subpath, so a shallow copy
    // plus that subpath's length is a complete snapshot of the path mirror.
    var subs = this._pathSubpaths;
    return {
        state: state,
        sent: sent,
        transform: this._currentTransform.slice(),
        subpaths: subs.slice(),
        lastSubpathLength: subs.length > 0 ? subs[subs.length - 1].length : 0,
        clipDepth: this._clipDepth
    };
};

CanvasRenderingContext2D.prototype._applyState = function(snap) {
    var sf = CanvasRenderingContext2D._STATE_FIELDS;
    for (var i = 0; i < sf.length; ++i) this[sf[i]] = snap.state[sf[i]];
    if (snap.sent) {
        var cf = CanvasRenderingContext2D._SENT_FIELDS;
        for (var k = 0; k < cf.length; ++k) this[cf[k]] = snap.sent[cf[k]];
    } else {
        // The native command stream was replaced while this snapshot was on
        // the stack, so nothing it recorded is still on the canvas.
        this._clearSentState();
    }
    this._currentTransform = snap.transform;
    var subs = snap.subpaths;
    if (subs.length > 0 && subs[subs.length - 1].length !== snap.lastSubpathLength) {
        subs[subs.length - 1] = subs[subs.length - 1].slice(0, snap.lastSubpathLength);
    }
    this._pathSubpaths = subs;
    this._clipDepth = snap.clipDepth || 0;
};

// ── Cached groups (Pulp extension, not Canvas2D) ──────────────────────────
//
// ctx.pulpCachedGroup(key, drawFn) draws static content once and replays it
// by reference: while `key` stays valid the call is one bridge crossing and
// drawFn does not run. The group replays in place, under the current
// transform and in the stream's blend order, so its pixels are exactly those
// of calling drawFn directly. It runs inside an implicit save()/restore(), so
// nothing it sets leaks into later draws, on either the recording frame or a
// replay.
//
// The cached content is a function of the key: drawFn is not re-run while the
// key is valid, so a group whose content depends on anything else must set
// that state itself or be invalidated with ctx.pulpInvalidateGroup(key) when
// it changes. State the enclosing code assigned before the call (fillStyle,
// font, ...) is sent first and inherited on every replay. Resizing the canvas
// and creating its context drop every group.
CanvasRenderingContext2D.prototype.pulpCachedGroup = function(key, drawFn) {
    this._fp();
    this._flushCachedState();
    key = String(key);
    if (!this._groupRecording && typeof canvasReplayGroup === "function") {
        if (canvasReplayGroup(this._id, key)) return;
        if (canvasBeginGroup(this._id, key)) {
            // The native group brackets itself in save/restore; mirror only
            // the JS side here so no save/restore command enters the stream.
            var snap = this._captureState();
            var depth = this._stateStack.length, floor = this._stateFloor;
            var completed = false;
            this._groupRecording = true;
            this._stateFloor = depth;
            try {
                drawFn(this);
                completed = true;
            } finally {
                this._fp();
                this._groupRecording = false;
                this._stateFloor = floor;
                // Saves the group left open close with it, as they do natively.
                this._stateStack.length = depth;
                this._applyState(snap);
                canvasEndGroup(this._id);
                // A drawFn that threw left a partial group; do not keep it.
                if (!completed) canvasInvalidateGroup(this._id, key);
            }
            return;
        }
    }
    // A group inside a recording group, or a host without cached groups:
    // draw directly with the same scoping.
    var outer = this._stateStack.length, outerFloor = this._stateFloor;
    this.save();
    this._stateFloor = outer + 1;
    try {
        drawFn(this);
    } finally {
        this._stateFloor = outerFloor;
        while (this._stateStack.length > outer) this.restore();
    }
};

// Drop the cached group `key`, or every group on this canvas when called
// with no key, so the next pulpCachedGroup call runs drawFn again.
CanvasRenderingContext2D.prototype.pulpInvalidateGroup = function(key) {
    this._fp();
    if (typeof canvasInvalidateGroup !== "function") return;
    if (key === undefined) canvasInvalidateGroup(this._id);
    else canvasInvalidateGroup(this._id, String(key));
};

// ── Canvas2D transform methods ────────────────────────────────────────────
//
// Each mutator updates the JS-mirrored `_currentTransform` in addition to
// forwarding to the bridge so getTransform() can return the live matrix
// without a round-trip. Composition rules:
//   translate(x,y):  M' = M * T(x,y)   →  e += a*x + c*y; f += b*x + d*y
//   scale(sx,sy):    M' = M * S(sx,sy) →  a *= sx; b *= sx; c *= sy; d *= sy
//   rotate(theta):   M' = M * R(theta) →  cos/sin block applied to (a,b,c,d)
//   setTransform:    replace
//   transform:       M' = M * given (concat-on-right; forwarded as the
//                    composed matrix via canvasSetTransform).
CanvasRenderingContext2D.prototype.translate = function(x, y) {
    this._fp();
    if (typeof canvasTranslate === "function") canvasTranslate(this._id, x, y);
    var t = this._currentTransform;
    t[4] += t[0] * x + t[2] * y;
    t[5] += t[1] * x + t[3] * y;
};
CanvasRenderingContext2D.prototype.scale = function(sx, sy) {
    this._fp();
    if (typeof canvasScale === "function") canvasScale(this._id, sx, sy);
    var t = this._currentTransform;
    t[0] *= sx; t[1] *= sx;
    t[2] *= sy; t[3] *= sy;
};
CanvasRenderingContext2D.prototype.rotate = function(radians) {
    this._fp();
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
    this._setBridgeTransform(a, b, c, d, e, f);
    this._currentTransform = [a, b, c, d, e, f];
};
CanvasRenderingContext2D.prototype.resetTransform = function() {
    this._setBridgeTransform(1, 0, 0, 1, 0, 0);
    this._currentTransform = [1, 0, 0, 1, 0, 0];
};
// getTransform() returns a DOMMatrix-shaped object reflecting the current
// 2D affine transform. HTML5 spec: returns a NEW DOMMatrix each call
// (mutating the returned object must not affect the live ctx transform),
// so we copy the live array into the matrix constructor.
CanvasRenderingContext2D.prototype.getTransform = function() {
    var t = this._currentTransform;
    return new _PulpCanvasMatrix(t[0], t[1], t[2], t[3], t[4], t[5]);
};
// transform(a,b,c,d,e,f) multiplies the current transform by the given
// matrix (concat-on-right), mirrors the result for synchronous queries, and
// forwards the composed matrix to the bridge.
CanvasRenderingContext2D.prototype.transform = function(a, b, c, d, e, f) {
    // Strict-concat semantics: M' = M * given (concat-on-right). Compose on
    // the JS-side mirror, then forward the full composed matrix to the
    // bridge so the canvas state stays in sync. canvasSetTransform replaces
    // the bridge state with the composed result, which equals M' below.
    var t = this._currentTransform;
    var na = t[0] * a + t[2] * b;
    var nb = t[1] * a + t[3] * b;
    var nc = t[0] * c + t[2] * d;
    var nd = t[1] * c + t[3] * d;
    var ne = t[0] * e + t[2] * f + t[4];
    var nf = t[1] * e + t[3] * f + t[5];
    this._setBridgeTransform(na, nb, nc, nd, ne, nf);
    this._currentTransform = [na, nb, nc, nd, ne, nf];
};

// ── Internal JS-side path-mirror helpers ──────────────────────────────────
// The bridge owns the canonical SkPath; these helpers maintain a parallel
// JS-side polyline approximation used by isPointInPath / isPointInStroke.
// Cubic and quadratic curves are sampled into ~16 line segments. Arc,
// arcTo, ellipse, and roundRect are recorded only in the bridge path today,
// so synchronous hit tests for those segments remain approximate/incomplete.
CanvasRenderingContext2D.prototype._pathMirrorMoveTo = function(x, y) {
    this._pathSubpaths.push([+x, +y]);
};
// Spec: a lineTo with no subpath behaves as moveTo.
CanvasRenderingContext2D.prototype._pathMirrorLineTo = function(x, y) {
    var subs = this._pathSubpaths;
    if (subs.length === 0) subs.push([+x, +y]);
    else subs[subs.length - 1].push(+x, +y);
};
// The current subpath, or null when the path is empty.
CanvasRenderingContext2D.prototype._pathMirrorLast = function() {
    var subs = this._pathSubpaths;
    if (subs.length === 0) return null;
    var last = subs[subs.length - 1];
    return last.length >= 2 ? last : null;
};
CanvasRenderingContext2D.prototype._pathMirrorCubic = function(c1x, c1y, c2x, c2y, x, y) {
    var last = this._pathMirrorLast();
    if (!last) { this._pathMirrorMoveTo(x, y); return; }
    var x0 = last[last.length - 2], y0 = last[last.length - 1];
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
    var last = this._pathMirrorLast();
    if (!last) { this._pathMirrorMoveTo(x, y); return; }
    var x0 = last[last.length - 2], y0 = last[last.length - 1];
    var STEPS = 16;
    for (var i = 1; i <= STEPS; ++i) {
        var t = i / STEPS;
        var u = 1 - t;
        var bx = u*u*x0 + 2*u*t*cx + t*t*x;
        var by = u*u*y0 + 2*u*t*cy + t*t*y;
        this._pathMirrorLineTo(bx, by);
    }
};

// ── Path methods (arc / rect / curves) ────────────────────────────────────
// Native bridge calls map to SkPath::arcTo (Skia) and CGPathAddArc (CG).
// Native arcs are closed-form correct for full circles, half circles, and
// the 3-collinear-points degenerate case; bezier approximations lose the
// exact-tangent property.
CanvasRenderingContext2D.prototype.arc = function(cx, cy, radius, startAngle, endAngle, anticlockwise) {
    this._fp();
    if (typeof canvasPathArc !== "function") return;
    canvasPathArc(this._id, cx, cy, radius, startAngle, endAngle,
                  anticlockwise ? 1 : 0);
};

CanvasRenderingContext2D.prototype.arcTo = function(x1, y1, x2, y2, radius) {
    this._fp();
    if (typeof canvasPathArcTo !== "function") return;
    canvasPathArcTo(this._id, x1, y1, x2, y2, radius);
};

CanvasRenderingContext2D.prototype.bezierCurveTo = function(c1x, c1y, c2x, c2y, x, y) {
    this._fp();
    if (typeof canvasCubicTo === "function") canvasCubicTo(this._id, c1x, c1y, c2x, c2y, x, y);
    this._pathMirrorCubic(c1x, c1y, c2x, c2y, x, y);
};

CanvasRenderingContext2D.prototype.quadraticCurveTo = function(cx, cy, x, y) {
    this._fp();
    if (typeof canvasQuadTo === "function") canvasQuadTo(this._id, cx, cy, x, y);
    this._pathMirrorQuad(cx, cy, x, y);
};

CanvasRenderingContext2D.prototype.rect = function(x, y, w, h) {
    // rect() is a path-construction op (not a draw). Emit four lineTos
    // back to the start point so the resulting subpath behaves like a
    // closed rectangle for fill()/stroke()/clip().
    if (typeof canvasMoveTo !== "function" || typeof canvasLineTo !== "function") return;
    // Route through the pending-run buffer so the rectangle joins the
    // current batched call, and so a following lineTo still appends to this
    // subpath.
    var coords = [+x, +y, x + w, +y, x + w, y + h, +x, y + h, +x, +y];
    this._openPendingSubpath(coords);
    this._pathSubpaths.push([+x, +y, x + w, +y, x + w, y + h, +x, y + h, +x, +y]);
};

CanvasRenderingContext2D.prototype.ellipse = function(cx, cy, rx, ry, rotation, startAngle, endAngle, anticlockwise) {
    this._fp();
    // Native ellipse via SkPath::arcTo + SkMatrix rotation (Skia) or
    // CGPathAddArc through a CGAffineTransform (CG). Honours `rotation`.
    if (typeof canvasPathEllipse !== "function") return;
    canvasPathEllipse(this._id, cx, cy, rx, ry, rotation || 0,
                      startAngle, endAngle, anticlockwise ? 1 : 0);
};

CanvasRenderingContext2D.prototype.roundRect = function(x, y, w, h, radii) {
    this._fp();
    // Native per-corner roundRect via SkRRect::MakeRectRadii (Skia) or
    // 8-segment CGPath layout (CG). The CSS spec accepts radii in five
    // forms — number, [r], [r1, r2], [r1, r2, r3], [r1, r2, r3, r4] — and
    // each can independently be a number (x==y) or an {x, y} object for an
    // elliptical corner. Normalize all forms to 8 floats before crossing
    // the bridge so the C++ side stays narrow.
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
    this._fp();
    // Match Canvas2D's clip() spec: intersect the current clip region with
    // the current path. Prefer canvasClip when available; it accepts the
    // fillRule as int_val (0 = nonzero, 1 = evenodd). canvasClipRect is the
    // older rect-only path.
    var rule = (fillRule === "evenodd") ? 1 : 0;
    if (typeof canvasClip === "function") canvasClip(this._id, rule);
    this._clipDepth += 1;
};

// ── isPointInPath / isPointInStroke ──────────────────────────────────────
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
// intersecting paths (the FilterBank / synth UI use case). True winding-
// number semantics on self-intersecting paths are not currently supported by
// this JS approximation.
CanvasRenderingContext2D.prototype._pointInSubpath = function(subpath, x, y) {
    var inside = false;
    var n = subpath.length >> 1;
    if (n < 2) return false;
    for (var i = 0, j = n - 1; i < n; j = i++) {
        var xi = subpath[2 * i], yi = subpath[2 * i + 1];
        var xj = subpath[2 * j], yj = subpath[2 * j + 1];
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
        for (var j = 2; j + 1 < sp.length; j += 2) {
            var ax = sp[j - 2], ay = sp[j - 1];
            var bx = sp[j],     by = sp[j + 1];
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

// ── Text drawing ──────────────────────────────────────────────────────────
CanvasRenderingContext2D.prototype.fillText = function(text, x, y, maxWidth) {
    this._fp();
    this._syncGlobalState();
    this._syncShadowState();
    this._syncFilterState();
    this._syncDirectionState();
    this._syncTextState();
    this._applyFillStyle();
    // canvasFillText takes (id, text, x, y, size, color, maxWidth). When
    // the active fillStyle is a gradient the bridge keeps the gradient
    // active on the canvas; canvasFillText still records a color, so
    // pass the gradient's first stop as a graceful approximation.
    var color = this.fillStyle;
    if (color && color._kind) {
        color = (color._stops && color._stops.length > 0) ? color._stops[0].color : "#fff";
    }
    // Parse `<size>` from the full CSS font shorthand. The family/weight/
    // slant already flowed through canvasSetFontFull during _syncTextState;
    // canvasFillText only needs the size for its own baseline math.
    var parsed = CanvasRenderingContext2D._parseFontShorthand(this.font || "14px Inter");
    // Canvas2D `fillText(text, x, y, maxWidth)`. Thread the optional
    // maxWidth through to the bridge as a 7th arg in CSS px. Spec: `<= 0`,
    // NaN, Infinity, or undefined all mean "no constraint", so we coerce
    // to a finite positive number or 0 (the bridge sentinel).
    var mw = __pulpCanvasPositiveFiniteOrZero(maxWidth);
    if (typeof canvasFillText === "function") {
        canvasFillText(this._id, String(text == null ? "" : text), x, y, parsed.size, String(color), mw);
        // The native fill_text sets the fill colour it carries.
        this._sentFillColor = String(color);
    }
};

CanvasRenderingContext2D.prototype.strokeText = function(text, x, y, maxWidth) {
    this._fp();
    // True outlined-glyph rendering when the host has canvasStrokeText.
    // On older hosts without this bridge entry, fall back to the
    // fillText-with-strokeColor approximation so the call doesn't throw.
    this._syncGlobalState();
    this._syncShadowState();
    this._syncTextState();
    // strokeText uses strokeStyle, not fillStyle. Push the active
    // strokeStyle + lineWidth through the bridge so Canvas::stroke_text
    // picks them up when it builds its stroke paint.
    this._applyStrokeStyle();
    var color = this.strokeStyle;
    if (color && color._kind) {
        // strokeStyle gradient is bridged through _applyStrokeStyle's
        // per-kind setter (canvasSetStrokeLinearGradient, etc.), which
        // populates stroke_shader_ on the Skia canvas. The color passed to
        // canvasStrokeText is only a solid fallback if stroke_shader_ is null
        // at paint time, so use the first stop here.
        color = (color._stops && color._stops.length > 0) ? color._stops[0].color : "#fff";
    }
    var parsed = CanvasRenderingContext2D._parseFontShorthand(this.font || "14px Inter");
    var mw = __pulpCanvasPositiveFiniteOrZero(maxWidth);
    if (typeof canvasStrokeText === "function") {
        canvasStrokeText(this._id, String(text == null ? "" : text), x, y, parsed.size, String(color), mw);
        return;
    }
    // Backwards-compat fallback: older hosts route stroke through fillText
    // with strokeStyle as the fill color. Visually close enough for
    // HUD/Filterbank text on legacy builds.
    var savedFill = this.fillStyle;
    this.fillStyle = this.strokeStyle;
    try { this.fillText(text, x, y, maxWidth); }
    finally { this.fillStyle = savedFill; }
};

// ── Gradient factories ────────────────────────────────────────────────────
CanvasRenderingContext2D.prototype.createLinearGradient = function(x0, y0, x1, y1) {
    return new CanvasGradient("linear", { x0: x0, y0: y0, x1: x1, y1: y1 });
};

CanvasRenderingContext2D.prototype.createRadialGradient = function(x0, y0, r0, x1, y1, r1) {
    // Carry both circles in the gradient handle; the fillStyle flush picks
    // the two-circle bridge when available and falls back to the
    // single-circle outer-only path on older binaries. Skia routes through
    // SkGradientShader::MakeTwoPointConical; CG routes through
    // CGContextDrawRadialGradient with both circles.
    return new CanvasGradient("radial", {
        x0: +x0 || 0, y0: +y0 || 0, r0: +r0 || 0,
        x1: +x1 || 0, y1: +y1 || 0, r1: +r1 || 0
    });
};

CanvasRenderingContext2D.prototype.createConicGradient = function(startAngle, cx, cy) {
    // Build a conic CanvasGradient. Spec signature:
    // createConicGradient(startAngle, x, y) where startAngle is in radians.
    // Skia renders the sweep via SkGradientShader::MakeSweep; CoreGraphics
    // software-rasterizes the sweep at paint time. The gradient is flushed via
    // canvasSetConicGradient when assigned to ctx.fillStyle.
    return new CanvasGradient("conic", {
        cx: +cx || 0,
        cy: +cy || 0,
        startAngle: +startAngle || 0
    });
};

CanvasRenderingContext2D.prototype.createPattern = function(image, repetition) {
    // Return a CanvasPattern handle that ctx.fillStyle / ctx.strokeStyle
    // accept; _applyFillStyle flushes via canvasSetFillPattern when a
    // pattern is the active fillStyle. Spec repetition values:
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
    // image-like object with .src / ._src (matches drawImage normalization).
    var src = "";
    if (typeof image === "string") src = image;
    else if (image && typeof image.src === "string") src = image.src;
    else if (image && typeof image._src === "string") src = image._src;
    // Spec: returning null is permissible when the source is unavailable.
    // We require a non-empty src to flush meaningful state to the bridge.
    if (!src) return null;
    return new CanvasPattern(src, tx, ty);
};
