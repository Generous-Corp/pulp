// ═══════════════════════════════════════════════════════════════════════════════
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
        if (!this._canvasContextWebgpu && typeof __createMockGPUCanvasContext === "function") {
            this._canvasContextWebgpu = __createMockGPUCanvasContext(this);
        }
        return this._canvasContextWebgpu || null;
    }
    return null;
};
