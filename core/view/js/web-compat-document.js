// ═══════════════════════════════════════════════════════════════════════════════
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
    _listeners: {},

    // Document-scope listeners are a no-op in headless capture; OrbitControls
    // and similar controls call `domElement.ownerDocument.addEventListener`
    // for window-level events (mouseup-outside-canvas etc.) we don't dispatch.
    addEventListener: function() {},
    removeEventListener: function() {},
    dispatchEvent: function() { return true; },

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
    },

    addEventListener: function(type, fn) {
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(fn);
    },

    removeEventListener: function(type, fn) {
        var list = this._listeners[type];
        if (!list) return;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === fn) list.splice(i, 1);
        }
    },

    dispatchEvent: function(event) {
        var list = this._listeners[event.type];
        if (!list) return;
        for (var i = 0; i < list.length; i++) list[i](event);
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

if (typeof globalThis.window === "undefined") {
    globalThis.window = window;
}

if (typeof globalThis.self === "undefined") {
    globalThis.self = globalThis.window;
}

// Bind directly to the window-object's frame helpers — `var window = {…}`
// at script-top becomes a property of globalThis in V8, so resolving
// `globalThis.window.requestAnimationFrame` from a wrapper that itself lives
// on globalThis would recurse infinitely (RangeError: Maximum call stack size
// exceeded — silently swallowed by Three.js's `new Promise(async () => {…})`
// async-executor inside renderer.init, leaving the init promise pending forever).
var __pulpWindowRequestFrame__ = window.requestAnimationFrame;
var __pulpWindowCancelFrame__ = window.cancelAnimationFrame;
if (typeof globalThis.requestAnimationFrame === "undefined") {
    globalThis.requestAnimationFrame = function(fn) {
        return __pulpWindowRequestFrame__(fn);
    };
}

if (typeof globalThis.cancelAnimationFrame === "undefined") {
    globalThis.cancelAnimationFrame = function(id) {
        return __pulpWindowCancelFrame__(id);
    };
}

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

__installGlobalIfMissing("GPUShaderStage", {
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

function __mockTextureBytesPerPixel(format) {
    switch (String(format || __mockPreferredCanvasFormat())) {
        case "rgba8unorm":
        case "bgra8unorm":
        case "rgba8unorm-srgb":
        case "bgra8unorm-srgb":
            return 4;
        default:
            return 4;
    }
}

function __allocateMockTextureBytes(texture) {
    var bytesPerPixel = __mockTextureBytesPerPixel(texture && texture.format);
    var width = texture && texture.width ? texture.width : 1;
    var height = texture && texture.height ? texture.height : 1;
    var depthOrArrayLayers = texture && texture.depthOrArrayLayers ? texture.depthOrArrayLayers : 1;
    return new Uint8Array(width * height * depthOrArrayLayers * bytesPerPixel);
}

function __createMockGPUBuffer(init) {
    init = init || {};
    var buffer = {
        _objectName: "GPUBuffer",
        label: init.label || "",
        size: init.size || 0,
        usage: init.usage || 0,
        mapState: init.mappedAtCreation ? "mapped" : "unmapped",
        _destroyed: false,
        _bytes: new Uint8Array(init.size || 0),
        _mappedRanges: []
    };
    buffer.mapAsync = function() {
        buffer.mapState = "mapped";
        buffer._mappedRanges = [];
        return Promise.resolve(undefined);
    };
    buffer.getMappedRange = function(offset, size) {
        var begin = offset || 0;
        var end = size == null ? buffer.size : begin + size;
        var mapped = buffer._bytes.slice(begin, end).buffer;
        buffer._mappedRanges.push({
            begin: begin,
            end: end,
            bytes: mapped
        });
        return mapped;
    };
    buffer.unmap = function() {
        for (var i = 0; i < buffer._mappedRanges.length; ++i) {
            var range = buffer._mappedRanges[i];
            buffer._bytes.set(new Uint8Array(range.bytes), range.begin);
        }
        buffer._mappedRanges = [];
        buffer.mapState = "unmapped";
    };
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
        baseMipLevel: init.baseMipLevel == null ? 0 : init.baseMipLevel,
        mipLevelCount: init.mipLevelCount == null ? 1 : init.mipLevelCount,
        baseArrayLayer: init.baseArrayLayer == null ? 0 : init.baseArrayLayer,
        arrayLayerCount: init.arrayLayerCount == null ? 1 : init.arrayLayerCount,
        texture: init.texture || null,
        _nativeBridge: !!init.nativeBridge,
        _nativeCanvasId: init.nativeCanvasId || "",
        _nativeTextureId: init.nativeTextureId || ""
    };
}

function __createMockGPUTexture(init) {
    init = init || {};
    var size = __textureExtent(init.size);
    var format = init.format || __mockPreferredCanvasFormat();
    var bytesPerPixel = __mockTextureBytesPerPixel(format);
    var texture = {
        _objectName: "GPUTexture",
        label: init.label || "",
        width: size.width,
        height: size.height,
        depthOrArrayLayers: size.depthOrArrayLayers,
        dimension: init.dimension || "2d",
        format: format,
        usage: init.usage || GPUTextureUsage.RENDER_ATTACHMENT,
        mipLevelCount: init.mipLevelCount || 1,
        sampleCount: init.sampleCount || 1,
        _destroyed: false,
        _nativeBridge: !!init.nativeBridge,
        _nativeCanvasId: init.nativeCanvasId || "",
        _nativeTextureId: init.nativeTextureId || "",
        _bytesPerRow: init.bytesPerRow || (size.width * bytesPerPixel),
        _rowsPerImage: init.rowsPerImage || size.height,
        _bytes: init.bytes ? __toUint8Array(init.bytes) : null
    };
    if (!texture._bytes) {
        texture._bytes = __allocateMockTextureBytes(texture);
    }
        texture.createView = function(descriptor) {
            descriptor = descriptor || {};
            return __createMockGPUTextureView({
                label: descriptor.label || texture.label,
                format: descriptor.format || texture.format,
                dimension: descriptor.dimension || texture.dimension,
                aspect: descriptor.aspect || "all",
                baseMipLevel: descriptor.baseMipLevel == null ? 0 : descriptor.baseMipLevel,
                mipLevelCount: descriptor.mipLevelCount == null ? 1 : descriptor.mipLevelCount,
                baseArrayLayer: descriptor.baseArrayLayer == null ? 0 : descriptor.baseArrayLayer,
                arrayLayerCount: descriptor.arrayLayerCount == null ? 1 : descriptor.arrayLayerCount,
                texture: texture,
                nativeBridge: texture._nativeBridge,
                nativeCanvasId: texture._nativeCanvasId,
                nativeTextureId: texture._nativeTextureId
            });
        };
    texture.destroy = function() {
        if (texture._nativeBridge && texture._nativeTextureId &&
            typeof __gpuDestroyTextureImpl === "function") {
            __gpuDestroyTextureImpl(texture._nativeTextureId);
        }
        texture._destroyed = true;
    };
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
        end: function() {
            if (typeof init.onEnd !== "function") return;
            var descriptor = init.descriptor || {};
            var attachments = descriptor.colorAttachments || [];
            var attachment = attachments.length > 0 ? attachments[0] : null;
            var view = attachment && attachment.view ? attachment.view : null;
            var clearValue = attachment && attachment.clearValue ? attachment.clearValue : null;
            var loadOp = attachment && attachment.loadOp ? attachment.loadOp : "";
            if (view && view._nativeBridge && view._nativeCanvasId && loadOp === "clear" && clearValue) {
                init.onEnd({
                    type: "native-clear-current-texture",
                    canvasId: view._nativeCanvasId,
                    r: Number(clearValue.r == null ? 0 : clearValue.r),
                    g: Number(clearValue.g == null ? 0 : clearValue.g),
                    b: Number(clearValue.b == null ? 0 : clearValue.b),
                    a: Number(clearValue.a == null ? 1 : clearValue.a)
                });
                return;
            }
            init.onEnd(null);
        }
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
    var encoder = {
        _objectName: "GPUCommandEncoder",
        label: init.label || "",
        _commands: [],
        beginRenderPass: function(descriptor) {
            return __createMockGPURenderPassEncoder({
                label: descriptor && descriptor.label ? descriptor.label : "",
                descriptor: descriptor || {},
                onEnd: function(command) {
                    if (command) encoder._commands.push(command);
                }
            });
        },
        beginComputePass: function(descriptor) {
            return __createMockGPUComputePassEncoder({ label: descriptor && descriptor.label ? descriptor.label : "" });
        },
        copyBufferToBuffer: function() {},
        copyTextureToBuffer: function() {},
        copyBufferToTexture: function() {},
        finish: function(descriptor) {
            var commandBuffer = __createMockGPUCommandBuffer({ label: descriptor && descriptor.label ? descriptor.label : "" });
            commandBuffer._commands = encoder._commands.slice();
            return commandBuffer;
        }
    };
    return encoder;
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
        _bindGroupLayouts: init.bindGroupLayouts || [],
        _nativeBridge: !!init.nativeBridge,
        vertex: init.vertex || {},
        fragment: init.fragment || {},
        primitive: init.primitive || {}
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
        addressModeW: init.addressModeW || "clamp-to-edge",
        magFilter: init.magFilter || "nearest",
        minFilter: init.minFilter || "nearest",
        mipmapFilter: init.mipmapFilter || "nearest"
    };
}

function __createMockGPUQueue(init) {
    init = init || {};
    var queue = {
        _objectName: "GPUQueue",
        label: init.label || "",
        _submitCount: 0,
        _nativeBridge: !!init.nativeBridge
    };
    queue.submit = function(commandBuffers) {
        queue._submitCount += commandBuffers && typeof commandBuffers.length === "number" ? commandBuffers.length : 0;
        if (!queue._nativeBridge || typeof __gpuQueueSubmitImpl !== "function" || !commandBuffers) return;
        for (var i = 0; i < commandBuffers.length; ++i) {
            var commandBuffer = commandBuffers[i];
            var commands = commandBuffer && commandBuffer._commands ? commandBuffer._commands : [];
            for (var j = 0; j < commands.length; ++j) {
                var command = commands[j];
                if (command && command.type === "native-clear-current-texture") {
                    __gpuQueueSubmitImpl(command.canvasId, command.r, command.g, command.b, command.a);
                }
            }
        }
    };
    queue.writeBuffer = function(buffer, bufferOffset, data, dataOffset, size) {
        if (!buffer || buffer._objectName !== "GPUBuffer") return;
        var source = __toUint8Array(data);
        var begin = bufferOffset || 0;
        var sliceOffset = dataOffset || 0;
        var isTypedArray = typeof ArrayBuffer !== "undefined" &&
            typeof ArrayBuffer.isView === "function" &&
            ArrayBuffer.isView(data) &&
            !(typeof DataView !== "undefined" && data instanceof DataView) &&
            data && typeof data.BYTES_PER_ELEMENT === "number";
        var byteOffset = isTypedArray ? sliceOffset * data.BYTES_PER_ELEMENT : sliceOffset;
        var byteSize = size == null
            ? source.length - byteOffset
            : (isTypedArray ? size * data.BYTES_PER_ELEMENT : size);
        buffer._bytes.set(source.slice(byteOffset, byteOffset + byteSize), begin);
    };
    queue.writeTexture = function(destination, data, dataLayout, size) {
        destination = destination || {};
        dataLayout = dataLayout || {};
        var texture = destination.texture || null;
        if (!texture || texture._objectName !== "GPUTexture") return;

        var source = __toUint8Array(data);
        var extent = __textureExtent(size || texture);
        var bytesPerPixel = __mockTextureBytesPerPixel(texture.format);
        var bytesPerRow = dataLayout.bytesPerRow || (extent.width * bytesPerPixel);
        var rowsPerImage = dataLayout.rowsPerImage || extent.height;
        var origin = destination.origin || {};
        var originX = origin.x || 0;
        var originY = origin.y || 0;
        var originZ = origin.z || 0;

        if (!texture._bytes || !(texture._bytes instanceof Uint8Array)) {
            texture._bytes = __allocateMockTextureBytes(texture);
        }
        texture._bytesPerRow = texture.width * bytesPerPixel;
        texture._rowsPerImage = texture.height;

        var copyWidthBytes = extent.width * bytesPerPixel;
        for (var z = 0; z < extent.depthOrArrayLayers; ++z) {
            for (var y = 0; y < extent.height; ++y) {
                var srcOffset = z * rowsPerImage * bytesPerRow + y * bytesPerRow;
                var dstRow = (originZ + z) * texture._rowsPerImage + (originY + y);
                var dstOffset = dstRow * texture._bytesPerRow + originX * bytesPerPixel;
                var slice = source.slice(srcOffset, srcOffset + copyWidthBytes);
                texture._bytes.set(slice, dstOffset);
            }
        }
    };
    queue.copyExternalImageToTexture = function() {};
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
    }
    return picked;
}

function __createMockGPUDevice(adapter, descriptor, init) {
    descriptor = descriptor || {};
    init = init || {};
    var device = {
        _objectName: "GPUDevice",
        label: descriptor.label || init.label || "",
        _nativeBridge: !!init.nativeBridge,
        features: __createFeatureSet(init.features || __pickDeviceFeatures(adapter, descriptor)),
        limits: __mergeMockGpuLimits(init.limits || descriptor.requiredLimits),
        queue: __createMockGPUQueue({ nativeBridge: !!init.nativeBridge }),
        adapterInfo: init.adapterInfo || (adapter && adapter.info ? adapter.info : null),
        lost: new Promise(function() {}),
        _destroyed: false,
        _errorScopes: [],
        _eventListeners: {}
    };
    device.createBuffer = function(bufferDescriptor) { return __createMockGPUBuffer(bufferDescriptor || {}); };
    device.createTexture = function(textureDescriptor) {
        textureDescriptor = textureDescriptor || {};
        var nativeTextureId = "";
        if (device._nativeBridge && typeof __gpuCreateTextureImpl === "function") {
            var size = __textureExtent(textureDescriptor.size);
            nativeTextureId = String(__gpuCreateTextureImpl(JSON.stringify({
                label: textureDescriptor.label || "",
                size: {
                    width: size.width,
                    height: size.height,
                    depthOrArrayLayers: size.depthOrArrayLayers
                },
                dimension: textureDescriptor.dimension || "2d",
                format: textureDescriptor.format || __mockPreferredCanvasFormat(),
                usage: textureDescriptor.usage || GPUTextureUsage.RENDER_ATTACHMENT,
                mipLevelCount: textureDescriptor.mipLevelCount || 1,
                sampleCount: textureDescriptor.sampleCount || 1
            })) || "");
        }
        return __createMockGPUTexture({
            label: textureDescriptor.label || "",
            size: textureDescriptor.size,
            dimension: textureDescriptor.dimension || "2d",
            format: textureDescriptor.format || __mockPreferredCanvasFormat(),
            usage: textureDescriptor.usage || GPUTextureUsage.RENDER_ATTACHMENT,
            mipLevelCount: textureDescriptor.mipLevelCount || 1,
            sampleCount: textureDescriptor.sampleCount || 1,
            bytesPerRow: textureDescriptor.bytesPerRow,
            rowsPerImage: textureDescriptor.rowsPerImage,
            bytes: textureDescriptor.bytes,
            nativeBridge: device._nativeBridge && !!nativeTextureId,
            nativeTextureId: nativeTextureId
        });
    };
    device.createSampler = function(samplerDescriptor) { return __createMockGPUSampler(samplerDescriptor || {}); };
    device.createShaderModule = function(shaderDescriptor) { return __createMockGPUShaderModule(shaderDescriptor || {}); };
    device.createBindGroupLayout = function(layoutDescriptor) { return __createMockGPUBindGroupLayout(layoutDescriptor || {}); };
    device.createBindGroup = function(bindGroupDescriptor) { return __createMockGPUBindGroup(bindGroupDescriptor || {}); };
    device.createPipelineLayout = function(layoutDescriptor) { return __createMockGPUPipelineLayout(layoutDescriptor || {}); };
    device.createRenderPipeline = function(pipelineDescriptor) {
        pipelineDescriptor = pipelineDescriptor || {};
        return __createMockGPURenderPipeline({
            label: pipelineDescriptor.label || "",
            nativeBridge: device._nativeBridge,
            vertex: pipelineDescriptor.vertex || {},
            fragment: pipelineDescriptor.fragment || {},
            primitive: pipelineDescriptor.primitive || {},
            bindGroupLayouts: pipelineDescriptor.layout && pipelineDescriptor.layout.bindGroupLayouts
                ? pipelineDescriptor.layout.bindGroupLayouts : []
        });
    };
    device.createRenderPipelineAsync = function(pipelineDescriptor) {
        return Promise.resolve(device.createRenderPipeline(pipelineDescriptor || {}));
    };
    device.pushErrorScope = function(filter) {
        device._errorScopes.push(filter == null ? "validation" : String(filter));
    };
    device.popErrorScope = function() {
        if (device._errorScopes.length > 0) {
            device._errorScopes.pop();
        }
        return Promise.resolve(null);
    };
    device.addEventListener = function(type, callback) {
        type = String(type || "");
        if (!device._eventListeners[type]) device._eventListeners[type] = [];
        if (typeof callback === "function") device._eventListeners[type].push(callback);
    };
    device.removeEventListener = function(type, callback) {
        type = String(type || "");
        var listeners = device._eventListeners[type];
        if (!listeners || listeners.length === 0) return;
        if (typeof callback !== "function") {
            device._eventListeners[type] = [];
            return;
        }
        var filtered = [];
        for (var i = 0; i < listeners.length; ++i) {
            if (listeners[i] !== callback) filtered.push(listeners[i]);
        }
        device._eventListeners[type] = filtered;
    };
    device.createCommandEncoder = function(commandDescriptor) { return __createMockGPUCommandEncoder(commandDescriptor || {}); };
    // WebGPU error-scope API used by Three.js's WebGPUPipelineUtils — return null
    // (no error) on pop. Matches spec shape; mock backend produces no GPU errors.
    device.pushErrorScope = function() {};
    device.popErrorScope = function() { return Promise.resolve(null); };
    device.destroy = function() { device._destroyed = true; };
    return device;
}

function __createGPUAdapter(init) {
    init = init || {};
    var adapter = {
        _objectName: "GPUAdapter",
        name: init.name || "Mock Dawn Adapter",
        backend: init.backend || __mockGpuInfo().backend,
        preferredCanvasFormat: init.preferredCanvasFormat || __mockPreferredCanvasFormat(),
        _nativeBridge: !!init.nativeBridge,
        features: __createFeatureSet(init.features || [ "core-features-and-limits", "timestamp-query" ]),
        limits: __mergeMockGpuLimits(init.limits),
        info: init.info || { vendor: "Pulp", architecture: init.backend || __mockGpuInfo().backend, description: init.name || "Mock Dawn Adapter" }
    };
    adapter.requestDevice = function(descriptor) {
        var initDescriptor = {};
        if (adapter._nativeBridge && typeof __describeNativeDeviceImpl === "function") {
            initDescriptor = __describeNativeDeviceImpl(descriptor || {}) || {};
        }
        return Promise.resolve(__createMockGPUDevice(adapter, descriptor || {}, initDescriptor));
    };
    return adapter;
}

function __createMockGPUAdapter(init) {
    return __createGPUAdapter(init || {});
}

var navigator = globalThis.navigator || {};
if (typeof navigatorGPU !== "undefined" && navigatorGPU) {
    navigator.gpu = navigatorGPU;
    navigator.gpu.requestAdapter = function() {
        var descriptor = null;
        if (typeof __describeNativeAdapterImpl === "function") {
            descriptor = __describeNativeAdapterImpl();
        }
        if (descriptor && descriptor.nativeBridge) {
            return Promise.resolve(__createGPUAdapter(descriptor));
        }
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
    this._map[String(name || "").toLowerCase()] = String(value == null ? "" : value);
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
    describeNativeAdapter: function() {
        if (typeof __describeNativeAdapterImpl === "function") return __describeNativeAdapterImpl();
        return null;
    },
    createMockAdapter: function() {
        var info = __mockGpuInfo();
        return __createMockGPUAdapter({
            backend: info.backend,
            preferredCanvasFormat: __mockPreferredCanvasFormat()
        });
    },
    createNativeAdapter: function() {
        var descriptor = window.pulp.gpu.describeNativeAdapter();
        if (!descriptor || !descriptor.nativeBridge) return null;
        return __createGPUAdapter(descriptor);
    },
    createMockDevice: function(adapter, descriptor) {
        adapter = adapter && adapter._objectName === "GPUAdapter" ? adapter : window.pulp.gpu.createMockAdapter();
        descriptor = descriptor || {};
        return __createMockGPUDevice(adapter, descriptor);
    }
};

// Three.js's Animation does `this._context = typeof self !== 'undefined' ? self : null`
// and then calls `this._context.requestAnimationFrame(...)`. Without `self`, it
// silently nulls out and the renderer.init's async-executor swallows the throw.
// Provide a minimal `self` shim with just rAF/cAF — assigning self=globalThis or
// self=window causes CHOC's v8ToChoc to stack-overflow on the cycle (Tracktion/choc#105).
if (typeof globalThis.self === "undefined") {
    globalThis.self = {
        requestAnimationFrame: window.requestAnimationFrame,
        cancelAnimationFrame: window.cancelAnimationFrame
    };
}

// OrbitControls (and other DOM-heavy libs) read `el.ownerDocument` to attach
// document-scoped event listeners. Define as a non-enumerable getter so
// CHOC's v8ToChoc property walker skips it (avoids the cycle bug, see #105).
if (typeof Element !== "undefined" && Element.prototype && !Object.getOwnPropertyDescriptor(Element.prototype, "ownerDocument")) {
    Object.defineProperty(Element.prototype, "ownerDocument", {
        get: function() { return document; },
        configurable: true,
        enumerable: false
    });
}
function _stripCssComments(text) {
    return text.replace(/\/\*[\s\S]*?\*\//g, "");
}

function _parseCssDeclarations(body) {
    var props = {};
    var decls = body.split(";");
    for (var i = 0; i < decls.length; i++) {
        var d = decls[i].trim();
        if (!d) continue;
        var colon = d.indexOf(":");
        if (colon <= 0) continue;
        var name = d.slice(0, colon).trim();
        var value = d.slice(colon + 1).trim();
        if (!name || !value) continue;
        // CSS uses kebab-case; CSSStyleDeclaration._props expects camelCase.
        // The setter side handles both, but normalize here so layered
        // overrides on the same logical property collapse correctly.
        var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
        props[camel] = value;
    }
    return props;
}

function _parseCssText(text) {
    // Returns an array of { selector, properties } records — array (not
    // object) so duplicate selectors layer in source order.
    var rules = [];
    var src = _stripCssComments(text || "");
    var i = 0;
    while (i < src.length) {
        // Skip whitespace and at-rule blocks (we don't support them; just
        // jump over the matching brace pair so a stray @media doesn't
        // poison the rest of the sheet).
        var ch = src.charAt(i);
        if (ch === " " || ch === "\t" || ch === "\n" || ch === "\r") { i++; continue; }
        if (ch === "@") {
            var depth = 0, found = false;
            while (i < src.length) {
                var c2 = src.charAt(i++);
                if (c2 === "{") { depth++; found = true; }
                else if (c2 === "}") { depth--; if (depth === 0 && found) break; }
                else if (c2 === ";" && !found) { break; } // at-rule with no block
            }
            continue;
        }
        // Read selector list up to '{'
        var brace = src.indexOf("{", i);
        if (brace < 0) break;
        var selectorList = src.slice(i, brace).trim();
        var endBrace = src.indexOf("}", brace + 1);
        if (endBrace < 0) break;
        var body = src.slice(brace + 1, endBrace);
        var props = _parseCssDeclarations(body);
        // Split selector list on commas (top-level only; we don't support
        // selectors with parenthesized commas in this slice).
        var selectors = selectorList.split(",");
        for (var s = 0; s < selectors.length; s++) {
            var sel = selectors[s].trim();
            if (!sel) continue;
            rules.push({ selector: sel, properties: props });
        }
        i = endBrace + 1;
    }
    return rules;
}

function _processStyleElement(el) {
    // Detach any previously-applied sheet so re-setting textContent
    // (React commits, hot-reload) replaces rather than stacks.
    if (el._appliedSheet && typeof el._appliedSheet.detach === "function") {
        el._appliedSheet.detach();
        el._appliedSheet = null;
    }
    var rules = _parseCssText(el._textContent || "");
    if (rules.length === 0) return;
    // StyleSheet's constructor takes { selector: properties }; we feed it
    // a raw _parsedRules list to preserve duplicate selectors and
    // source-order layering for `:hover` rules.
    var sheet = Object.create(StyleSheet.prototype);
    sheet._rules = {};
    sheet._attached = false;
    sheet._parsedRules = [];
    for (var i = 0; i < rules.length; i++) {
        var r = rules[i];
        sheet._parsedRules.push({
            selector: r.selector,
            properties: r.properties,
            parsed: _parseSelector(r.selector)
        });
    }
    sheet.attach();
    el._appliedSheet = sheet;
}

function _matchesPseudoClass(el, pseudo) {
    if (!pseudo) return true;
    var lower = pseudo.toLowerCase();

    // State-on-element pseudo-classes — read the bridge-maintained slot.
    if (lower === "disabled") return !!el._disabled;
    if (lower === "enabled")  return !el._disabled;
    if (lower === "checked")  return !!el._checked;
    if (lower === "hover")    return !!el._isHovered;
    if (lower === "focus")    return !!el._hasFocus;
    if (lower === "active")   return !!el._isActive;

    // DOM-position pseudo-classes. Need a parent to compute the index.
    var parent = el._parentElement;
    if (lower === "first-child") {
        return !!parent && parent._children && parent._children[0] === el;
    }
    if (lower === "last-child") {
        if (!parent || !parent._children) return false;
        return parent._children[parent._children.length - 1] === el;
    }
    if (lower === "only-child") {
        return !!parent && parent._children && parent._children.length === 1
            && parent._children[0] === el;
    }
    // pulp #1737 (Codex P2 followup #3 on #1779): `:root` matches the
    // document root element specifically (`__bodyElement__`), not any
    // element with no parent. The previous `!el._parentElement` check
    // also matched DETACHED elements (createElement before appendChild),
    // which leaked `:root { ... }` theme/layout styles into normal
    // nodes when they were later inserted. StyleSheet.attach() walks
    // every entry in `__elements__` so the bug surfaced for any element
    // created mid-stylesheet-life.
    //
    // Tied to the root via identity check — __bodyElement__ is the
    // synthetic body element this shim creates at the top of
    // web-compat-document.js. Detached elements still have a non-null
    // _parentElement once mounted (and even pre-mount they're never
    // === __bodyElement__), so this branch is safe.
    //
    // Catalog still doesn't claim :root for document.querySelector
    // because _findMatch starts traversal from root._children — the
    // root itself is never queued. So:
    //   * stylesheet `:root { color: red }` → applies to body (this branch).
    //   * document.querySelector(':root') → returns null (traversal
    //     never sees the root). Catalog supportedValues notes the gap.
    if (lower === "root") {
        return el === __bodyElement__;
    }
    if (lower === "empty") {
        return !el._children || el._children.length === 0;
    }

    // Functional pseudo-classes: `:not(<simple>)` and `:nth-child(N|2n|...)`.
    // Use raw `pseudo` (not lowercased) so the inner selector retains case
    // semantics for tag names + attribute values.
    var notMatch = pseudo.match(/^not\((.+)\)$/i);
    if (notMatch) {
        var inner = _parseSelector(notMatch[1]);
        return !_matchesSelector(el, inner);
    }
    var nthMatch = pseudo.match(/^nth-child\((.+)\)$/i);
    if (nthMatch) {
        if (!parent || !parent._children) return false;
        var idx = parent._children.indexOf(el);
        if (idx < 0) return false;
        return _matchesNth(idx + 1, nthMatch[1].trim());
    }
    var nthLast = pseudo.match(/^nth-last-child\((.+)\)$/i);
    if (nthLast) {
        if (!parent || !parent._children) return false;
        var lastIdx = parent._children.indexOf(el);
        if (lastIdx < 0) return false;
        return _matchesNth(parent._children.length - lastIdx, nthLast[1].trim());
    }

    // Unknown pseudo-class — explicit no-match (per CSS Selectors Level 4
    // forward-compat: unknown pseudo-classes match nothing rather than
    // refusing to parse).
    return false;
}

// pulp #1737 — :nth-child(N) argument parser. Accepts:
//   * `odd` / `even` (case-insensitive)
//   * a positive integer literal (`2`, `5`)
//   * `An+B` / `An-B` formula (e.g. `2n`, `2n+1`, `3n-1`, `-n+3`).

function _matchesNth(pos, arg) {
    var lower = arg.toLowerCase().replace(/\s+/g, "");
    if (lower === "odd")  return pos % 2 === 1;
    if (lower === "even") return pos % 2 === 0;
    // Plain integer.
    if (/^-?\d+$/.test(lower)) return pos === parseInt(lower, 10);
    // An+B form. Match `[A]n[+|-B]` where A and B are signed integers.
    // Both are optional; n is required.
    var m = lower.match(/^(-?\d*)n([+-]\d+)?$/);
    if (!m) return false;
    var aRaw = m[1];
    var bRaw = m[2] || "0";
    var a = aRaw === "" || aRaw === "+" ? 1 : (aRaw === "-" ? -1 : parseInt(aRaw, 10));
    var b = parseInt(bRaw, 10);
    if (a === 0) return pos === b;
    var k = (pos - b) / a;
    // Index must be a non-negative integer.
    return k >= 0 && Math.floor(k) === k;
}

// ═══════════════════════════════════════════════════════════════════════════════
// querySelector / querySelectorAll
// ═══════════════════════════════════════════════════════════════════════════════

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

