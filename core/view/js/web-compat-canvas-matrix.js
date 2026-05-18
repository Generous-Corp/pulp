// ═══════════════════════════════════════════════════════════════════════════════
// _PulpCanvasMatrix — Canvas2D DOMMatrix-compat helper
// ═══════════════════════════════════════════════════════════════════════════════
//
// P5-6 follow-up — extracted from web-compat-canvas.js (164-289).
//
// 2D affine + 3D-padded matrix with the snapshot-mutator semantics
// HTML5 Canvas requires:
//   * 2D fields (a/b/c/d/e/f) + 4×4 m11..m44 aliases
//   * is2D / isIdentity flags
//   * toFloat32Array / toFloat64Array / toJSON serialization
//   * multiplySelf / scaleSelf / rotateSelf / translateSelf mutators
//   * inverse() returns a new matrix (NaN-filled + is2D=false on
//     singular input, per Codex P2 follow-up on #1754).
//
// CanvasRenderingContext2D.getTransform() returns a new instance of
// this class — the only external reference, resolved lazily at call
// time so embed order between this prelude and web-compat-canvas.js
// is flexible.

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
    // Codex P2 follow-up on #1754: honor the actual is2D state. A
    // singular-inverse result has is2D=false; if toJSON hardcodes
    // true, JSON consumers can't distinguish inversion failure from
    // a successful 2D inverse.
    return { a: this.a, b: this.b, c: this.c, d: this.d,
             e: this.e, f: this.f, is2D: this.is2D !== false,
             isIdentity: this.isIdentity };
};

// pulp #1527 — DOMMatrix mutator methods. Snapshot semantics
// (mutating the returned matrix does NOT affect the live ctx — that
// matches HTML5 Canvas spec). The mutators are useful for offline
// transform calculations like
// `ctx.getTransform().translateSelf(x, y).inverse()` and are widely
// used in plugin code adapted from web canvas libraries (Three.js
// canvas adapter, Skia-canvas, etc.). Each *Self method mutates this
// matrix in place and returns this for chaining; inverse() returns a
// NEW matrix (or throws on singular).
_PulpCanvasMatrix.prototype._refreshAliases = function() {
    this.m11 = this.a; this.m12 = this.b;
    this.m21 = this.c; this.m22 = this.d;
    this.m41 = this.e; this.m42 = this.f;
    this.isIdentity = (this.a === 1 && this.b === 0 && this.c === 0 &&
                       this.d === 1 && this.e === 0 && this.f === 0);
};
_PulpCanvasMatrix.prototype.multiplySelf = function(other) {
    var a = this.a, b = this.b, c = this.c, d = this.d, e = this.e, f = this.f;
    var oa = other.a, ob = other.b, oc = other.c, od = other.d, oe = other.e, of = other.f;
    this.a = a * oa + c * ob;
    this.b = b * oa + d * ob;
    this.c = a * oc + c * od;
    this.d = b * oc + d * od;
    this.e = a * oe + c * of + e;
    this.f = b * oe + d * of + f;
    this._refreshAliases();
    return this;
};
_PulpCanvasMatrix.prototype.scaleSelf = function(sx, sy) {
    // Codex P2 on #1730: spec says scaleX defaults to 1 when omitted,
    // and scaleY defaults to scaleX when omitted. Without this, the
    // bare scaleSelf() call multiplies by undefined and produces NaN.
    if (sx === undefined) sx = 1;
    if (sy === undefined) sy = sx;
    this.a *= sx;
    this.b *= sx;
    this.c *= sy;
    this.d *= sy;
    this._refreshAliases();
    return this;
};
_PulpCanvasMatrix.prototype.rotateSelf = function(angle) {
    // Codex P1 on #1730: DOMMatrix.rotateSelf() takes degrees per spec
    // (https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateself). The
    // previous implementation fed `angle` directly into Math.cos/sin
    // (radians), so callers ported from browsers (e.g. rotateSelf(90)
    // expecting a quarter turn) got the wrong rotation.
    if (angle === undefined) angle = 0;
    var rad = angle * Math.PI / 180;
    var cos = Math.cos(rad), sin = Math.sin(rad);
    var a = this.a, b = this.b, c = this.c, d = this.d;
    this.a =  a * cos + c * sin;
    this.b =  b * cos + d * sin;
    this.c = -a * sin + c * cos;
    this.d = -b * sin + d * cos;
    this._refreshAliases();
    return this;
};
_PulpCanvasMatrix.prototype.translateSelf = function(tx, ty) {
    if (tx === undefined) tx = 0;
    if (ty === undefined) ty = 0;
    this.e += this.a * tx + this.c * ty;
    this.f += this.b * tx + this.d * ty;
    this._refreshAliases();
    return this;
};
_PulpCanvasMatrix.prototype.inverse = function() {
    // Codex P1 on #1730: spec says non-invertible matrices yield a
    // matrix with NaN components and `is2D = false` — they do NOT
    // throw. Throwing here breaks compat for callers that rely on
    // the standard non-throwing inverse contract.
    // https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-inverse
    var a = this.a, b = this.b, c = this.c, d = this.d, e = this.e, f = this.f;
    var det = a * d - b * c;
    if (det === 0 || !isFinite(det)) {
        // Codex P2 follow-up: spec says ALL 16 components become NaN
        // for a non-invertible matrix. The constructor only NaNs the
        // 2D aliases (m11..m22, m41..m42); m13, m14, m23, m24,
        // m31..m34, m43, m44 stay at constructor-default identity.
        // toFloat32Array() / toFloat64Array() would then return
        // mixed finite/NaN data, violating the contract.
        var nan = new _PulpCanvasMatrix(NaN, NaN, NaN, NaN, NaN, NaN);
        nan.m13 = NaN; nan.m14 = NaN;
        nan.m23 = NaN; nan.m24 = NaN;
        nan.m31 = NaN; nan.m32 = NaN; nan.m33 = NaN; nan.m34 = NaN;
        nan.m43 = NaN; nan.m44 = NaN;
        nan.is2D = false;
        return nan;
    }
    var ia =  d / det, ib = -b / det;
    var ic = -c / det, id =  a / det;
    var ie = (c * f - d * e) / det;
    var ifv = (b * e - a * f) / det;
    return new _PulpCanvasMatrix(ia, ib, ic, id, ie, ifv);
};
