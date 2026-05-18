// ═══════════════════════════════════════════════════════════════════════════════
// Canvas2D API gap closures (issue-916) — image / line-dash / metrics
// ═══════════════════════════════════════════════════════════════════════════════
//
// P5-6 follow-up — extracted from web-compat-canvas.js (1350-1581).
//
// Five prototype methods covering the HTML5 Canvas2D surfaces that the
// catalog flipped from DIVERGE → PASS in Wave 2:
//   * measureText — returns a TextMetrics object with width +
//     actualBoundingBox{Left,Right,Ascent,Descent} +
//     fontBoundingBox{Ascent,Descent} fields. Falls back to a 0.6em
//     character-width estimate when the bridge function isn't
//     available.
//   * drawImage — handles all three signatures (3/5/9-arg) with the
//     Image-id ↔ asset-url resolution that the bridge needs.
//   * setLineDash / getLineDash — round-trips the dash pattern array.
//   * getImageData / putImageData — Base64 round-trips an ImageData
//     buffer through the bridge.
//
// Embed order: loaded AFTER web-compat-canvas.js so the
// CanvasRenderingContext2D constructor is in scope when the prototype
// installs the per-method overrides.

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
// drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh).
// pulp #1737 — 9-arg source-rect form is now wired. The JS shim
// passes sx,sy,sw,sh as args[6..9] and the bridge routes through
// Canvas::draw_image_from_*_rect (Skia: SkCanvas::drawImageRect with
// src+dst rects). Sprite-sheet slicing without re-decoding.
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
    var sx, sy, sw, sh;
    var has_src_rect = false;
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
        sx = a; sy = b; sw = c; sh = d;
        dx = e; dy = f; dw = g; dh = h;
        has_src_rect = true;
    }
    if (has_src_rect) {
        canvasDrawImage(this._id, src, dx, dy, dw, dh, sx, sy, sw, sh);
    } else {
        canvasDrawImage(this._id, src, dx, dy, dw, dh);
    }
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

// pulp Wave 4 c2d cleanup — lineDashOffset getter/setter pair. Mutating
// between draws now re-pushes the dash pattern with the new phase so the
// next stroke picks up the shift. Spec: HTML5 Canvas2D
// `lineDashOffset` is the only sticky line-dash phase property; assigning
// to it must be observable on subsequent stroke() calls without
// re-issuing setLineDash.
Object.defineProperty(CanvasRenderingContext2D.prototype, "lineDashOffset", {
    configurable: true,
    enumerable: true,
    get: function() { return this._lineDashOffset || 0; },
    set: function(v) {
        var nv = +v;
        if (!isFinite(nv)) return; // HTML5: non-finite values ignored.
        this._lineDashOffset = nv;
        // Re-flush the active dash pattern so the phase change lands on
        // the next draw. Bridge tolerates an empty pattern (no-op stroke).
        if (typeof canvasSetLineDash === "function") {
            var pat = this._lineDash || [];
            canvasSetLineDash(this._id, pat, nv);
        }
    }
});

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

// putImageData(imageData, dx, dy)
//   or putImageData(imageData, dx, dy, dirtyX, dirtyY, dirtyW, dirtyH)
//
// Encodes the Uint8ClampedArray as base64 and hands it to the bridge for
// rasterization on the next paint.
//
// pulp #1737 — 7-arg sub-rect form: only the (dirtyX, dirtyY, dirtyW,
// dirtyH) sub-rectangle of the source ImageData is written, at
// destination (dx + dirtyX, dy + dirtyY). Per HTML5 spec the dirty rect
// is clamped to the ImageData bounds; if it ends up empty the call is
// a no-op. We slice the source pixels row-by-row in JS so the bridge
// contract stays the no-rect form (smaller buffer + adjusted dst+
// dimensions), which avoids passing a sub-rect across the bridge or
// rewriting the C++ paint path.
CanvasRenderingContext2D.prototype.putImageData = function(imageData, dx, dy,
                                                            dirtyX, dirtyY,
                                                            dirtyW, dirtyH) {
    if (!imageData || typeof canvasPutImageData !== "function") return;
    var srcData = imageData.data || [];
    var srcW = imageData.width  | 0;
    var srcH = imageData.height | 0;
    if (srcW <= 0 || srcH <= 0) return;

    // Resolve dirty-rect args. The 3-arg form means "whole ImageData".
    var has_dirty = arguments.length >= 7;
    var sx, sy, sw, sh;
    if (has_dirty) {
        sx = dirtyX | 0;
        sy = dirtyY | 0;
        sw = dirtyW | 0;
        sh = dirtyH | 0;
        // HTML5 spec: negative width/height inverts the rect. Normalize.
        if (sw < 0) { sx += sw; sw = -sw; }
        if (sh < 0) { sy += sh; sh = -sh; }
        // Clamp to source bounds.
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > srcW) sw = srcW - sx;
        if (sy + sh > srcH) sh = srcH - sy;
        if (sw <= 0 || sh <= 0) return; // empty dirty rect — no-op
    } else {
        sx = 0; sy = 0; sw = srcW; sh = srcH;
    }

    // Source-rect slice: copy `sw * sh` RGBA pixels starting at (sx, sy).
    // For the no-rect path we keep the original flat array to avoid the
    // copy cost on the hot path.
    var data, width, height;
    if (has_dirty) {
        var sliced_n = sw * sh * 4;
        data = new Array(sliced_n);
        var di = 0;
        for (var row = 0; row < sh; ++row) {
            var srcRowStart = ((sy + row) * srcW + sx) * 4;
            for (var col = 0; col < sw * 4; ++col) {
                data[di++] = srcData[srcRowStart + col] | 0;
            }
        }
        width = sw;
        height = sh;
    } else {
        data = srcData;
        width = srcW;
        height = srcH;
    }

    // Destination top-left for the 7-arg form is (dx + sx, dy + sy).
    var dstX = (dx | 0) + (has_dirty ? sx : 0);
    var dstY = (dy | 0) + (has_dirty ? sy : 0);

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
    canvasPutImageData(this._id, out, width, height, dstX, dstY);
};
