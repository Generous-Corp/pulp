// ═══════════════════════════════════════════════════════════════════════════════
// WebGPU mock factories (P5-7 follow-up — extracted from web-compat-document.js)
// ═══════════════════════════════════════════════════════════════════════════════
//
// The entire WebGPU mock surface used by the headless test harness +
// fallback adapter path. Originally lived inline in
// web-compat-document.js, but at 563 lines it was the biggest
// self-contained sub-system in that file. Extracted to keep the
// document TU around the 1k-line maintainability target.
//
// Embed order: loaded AFTER web-compat-document.js because
//   * GPUTextureUsage / GPUBufferUsage / GPUShaderStage / GPUColorWrite
//     / GPUMapMode are installed at the top of web-compat-document.js
//     (lines 522-560), and the mock factory bodies reference those
//     constants;
//   * `window.pulp.gpu` (which uses `__mockGpuInfo` /
//     `__createMockGPUAdapter` / `__createMockGPUDevice` /
//     `__createGPUAdapter`) is defined further down in
//     web-compat-document.js, but only references these helpers
//     inside method bodies — those resolve lazily when the methods
//     are invoked, so it's safe to define the helpers afterwards.

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
        mapState: init.mappedAtCreation ? "mapped" : "unmapped",
        _destroyed: false,
        _bytes: new Uint8Array(init.size || 0),
        // Outstanding mapped ranges handed out by getMappedRange(), flushed
        // back into _bytes on unmap(). See the getMappedRange comment.
        _mappedRanges: []
    };
    buffer.mapAsync = function() {
        buffer.mapState = "mapped";
        return Promise.resolve(undefined);
    };
    // WebGPU semantics: getMappedRange() returns an ArrayBuffer the caller
    // writes into, and unmap() commits those writes to the buffer's backing
    // store. An ArrayBuffer cannot alias a *sliced* view of _bytes, so the
    // previous implementation returned `_bytes.buffer.slice(...)` (an
    // independent COPY) with a no-op unmap — every write to a mapped range was
    // silently dropped. Three.js's WebGPUBackend uploads geometry through exactly this
    // path (`createBuffer({mappedAtCreation:true})` → `new T(getMappedRange())
    // .set(attr.array)` → `unmap()`), so vertex/index buffers arrived all-zero
    // at the native bridge and the cube collapsed to a degenerate point
    // (#3217). Uniforms survived only because they use queue.writeBuffer, which
    // writes _bytes directly. Fix: hand back a standalone range seeded from the
    // current backing bytes, record it, and copy each recorded range back into
    // _bytes on unmap().
    buffer.getMappedRange = function(offset, size) {
        var begin = offset || 0;
        var end = size == null ? buffer._bytes.length : begin + size;
        if (end > buffer._bytes.length) { end = buffer._bytes.length; }
        if (begin > end) { begin = end; }
        var range = new ArrayBuffer(end - begin);
        new Uint8Array(range).set(buffer._bytes.subarray(begin, end));
        buffer._mappedRanges.push({ begin: begin, range: range });
        return range;
    };
    buffer.unmap = function() {
        if (buffer._mappedRanges && buffer._mappedRanges.length) {
            for (var i = 0; i < buffer._mappedRanges.length; i++) {
                var r = buffer._mappedRanges[i];
                buffer._bytes.set(new Uint8Array(r.range), r.begin);
            }
            buffer._mappedRanges = [];
        }
        buffer.mapState = "unmapped";
    };
    buffer.destroy = function() {
        buffer._destroyed = true;
        buffer._mappedRanges = [];
    };
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
        texture: init.texture || null,
        // iOS-D.3c (#3217): propagate native-bridge identity from the
        // owning texture so encoder.draw's createBufferedDrawPayload check
        // (web-compat-gpu-buffered.js:194) sees attachmentView._nativeBridge
        // and routes the draw through the native bridge instead of the mock
        // no-op. Without this, Three.js's WebGPURenderer renders to a JS-
        // side mock that drops every command, and the offscreen wgpu::Texture
        // we hand Skia stays blank → invisible cube.
        _nativeBridge: !!init._nativeBridge,
        _nativeCanvasId: init._nativeCanvasId || "",
        _nativeTextureId: init._nativeTextureId || ""
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
            texture: texture,
            // iOS-D.3c (#3217): forward the texture's native-bridge ids so
            // the resulting view can route encoder.draw through the native
            // bridge path (see __createMockGPUTextureView above).
            _nativeBridge: !!texture._nativeBridge,
            _nativeCanvasId: texture._nativeCanvasId || "",
            _nativeTextureId: texture._nativeTextureId || ""
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
    var currentComputePipeline = null;
    var computeBindGroups = {};
    var computeCommands = [];

    return {
        _objectName: "GPUComputePassEncoder",
        label: init.label || "",
        _commands: computeCommands,
        setPipeline: function(pipeline) {
            currentComputePipeline = pipeline;
        },
        setBindGroup: function(index, bindGroup) {
            computeBindGroups[index == null ? 0 : index] = bindGroup || null;
        },
        dispatchWorkgroups: function(x, y, z) {
            computeCommands.push({
                type: "dispatch",
                pipeline: currentComputePipeline,
                bindGroups: Object.assign({}, computeBindGroups),
                workgroupCountX: x || 1,
                workgroupCountY: y || 1,
                workgroupCountZ: z || 1
            });
        },
        dispatchWorkgroupsIndirect: function(indirectBuffer, indirectOffset) {
            computeCommands.push({
                type: "dispatch-indirect",
                pipeline: currentComputePipeline,
                bindGroups: Object.assign({}, computeBindGroups),
                indirectBuffer: indirectBuffer,
                indirectOffset: indirectOffset || 0
            });
        },
        end: function() {
            // Commands are captured in _commands for native dispatch
        }
    };
}

function __createMockGPUCommandEncoder(init) {
    init = init || {};
    var computePasses = [];
    // iOS-D.3c (#3217): collect render-pass commands here so they survive
    // into the command buffer's `_commands` array. Without this, the
    // canvas-gpu / buffered shims would push commands into a per-encoder
    // `passCommands` and `encoder.end` would have nowhere to forward them
    // (init.onEnd was never wired), and queue.submit would see
    // `commandBuffer._commands.length === 0` even though Three.js had
    // issued draws. Wiring onEnd → renderCommands.push lets the native
    // queue.submit dispatcher route each draw through
    // __gpuQueueDrawBufferedImpl / __gpuQueueDrawImpl.
    var renderCommands = [];
    var collectRenderCommand = function(cmd) {
        if (cmd) renderCommands.push(cmd);
    };
    return {
        _objectName: "GPUCommandEncoder",
        label: init.label || "",
        _computePasses: computePasses,
        beginRenderPass: function(descriptor) {
            return __createMockGPURenderPassEncoder({
                label: descriptor && descriptor.label ? descriptor.label : "",
                descriptor: descriptor || {},
                onEnd: collectRenderCommand
            });
        },
        beginComputePass: function(descriptor) {
            var pass = __createMockGPUComputePassEncoder({ label: descriptor && descriptor.label ? descriptor.label : "" });
            computePasses.push(pass);
            return pass;
        },
        copyBufferToBuffer: function() {},
        copyTextureToBuffer: function() {},
        copyBufferToTexture: function() {},
        finish: function(descriptor) {
            var cmdBuf = __createMockGPUCommandBuffer({ label: descriptor && descriptor.label ? descriptor.label : "" });
            // Attach compute pass commands to the command buffer for native dispatch
            cmdBuf._computePasses = computePasses;
            // iOS-D.3c (#3217): hand the captured render-pass commands to
            // the command buffer so queue.submit can iterate them and call
            // the per-command native dispatcher.
            cmdBuf._commands = renderCommands.slice();
            return cmdBuf;
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
        // pulp #2101 — bridge flag the buffered-draw augmentation
        // (web-compat-gpu-buffered.js) reads to decide whether to forward
        // command buffers to Dawn. That file wraps queue.submit with the
        // actual __gpuQueueSubmitImpl / __gpuQueueDrawBufferedImpl
        // dispatch; we just need to expose the flag here.
        _nativeBridge: !!init.nativeBridge,
        _submitCount: 0
    };
    queue.submit = function(commandBuffers) {
        queue._submitCount += commandBuffers && typeof commandBuffers.length === "number" ? commandBuffers.length : 0;
    };
    queue.writeBuffer = function(buffer, bufferOffset, data, dataOffset, size) {
        if (!buffer || buffer._objectName !== "GPUBuffer") return;
        var source = __toUint8Array(data);
        var begin = bufferOffset || 0;
        // iOS-D.3c (#3217): WebGPU spec — when `data` is a TypedArray,
        // `dataOffset` and `size` are measured in ELEMENTS of that array,
        // not bytes (only ArrayBuffer/DataView use byte units). The old
        // code treated both as bytes, so Three.js's
        // `writeBuffer(buf, dst, Float32Array(16), 0, 16)` — meaning 16
        // floats = 64 bytes — copied only 16 BYTES = 4 floats = the first
        // column of each mat4x4. Every camera + per-mesh matrix arrived
        // 3/4 truncated, collapsing all geometry to a degenerate point and
        // producing a blank canvas. Scale element counts to bytes here.
        var bytesPerEl = (data && typeof data.BYTES_PER_ELEMENT === "number")
            ? data.BYTES_PER_ELEMENT
            : 1;
        var sliceOffset = (dataOffset || 0) * bytesPerEl;
        var sliceSize = size == null ? source.length - sliceOffset : size * bytesPerEl;
        buffer._bytes.set(source.slice(sliceOffset, sliceOffset + sliceSize), begin);
    };
    queue.writeTexture = function(destination, data, dataLayout, size) {
        if (!destination || !destination.texture) return;
        var texture = destination.texture;
        var source = __toUint8Array(data);
        dataLayout = dataLayout || {};
        var copySize = __textureExtent(size);
        var sourceOffset = dataLayout.offset || 0;
        var bytes = sourceOffset > 0 ? source.slice(sourceOffset) : source;
        texture._bytes = bytes;
        texture._bytesPerRow = dataLayout.bytesPerRow || 0;
        texture._rowsPerImage = dataLayout.rowsPerImage || copySize.height || texture.height || 1;
        if (texture._nativeBridge && texture._nativeTextureId && typeof __gpuQueueWriteTextureImpl === "function") {
            __gpuQueueWriteTextureImpl(JSON.stringify({
                textureId: texture._nativeTextureId,
                data: Array.from(bytes),
                bytesPerRow: texture._bytesPerRow,
                rowsPerImage: texture._rowsPerImage,
                width: copySize.width,
                height: copySize.height,
                depthOrArrayLayers: copySize.depthOrArrayLayers,
                mipLevel: destination.mipLevel || 0,
                origin: destination.origin || {}
            }));
        }
    };
    queue.copyExternalImageToTexture = function(source, destination, copySize) {
        if (!source || !destination || !destination.texture) return;
        var imageBitmap = source.source;
        if (!imageBitmap || !imageBitmap._decodedPixels) return;
        var texture = destination.texture;
        var extent = __textureExtent(copySize);
        texture._bytes = imageBitmap._decodedPixels;
        texture._bytesPerRow = imageBitmap.width * 4;
        texture._rowsPerImage = imageBitmap.height;
        texture.width = imageBitmap.width;
        texture.height = imageBitmap.height;
        if (texture._nativeBridge && texture._nativeTextureId && typeof __gpuQueueWriteTextureImpl === "function") {
            __gpuQueueWriteTextureImpl(JSON.stringify({
                textureId: texture._nativeTextureId,
                data: Array.from(texture._bytes),
                bytesPerRow: texture._bytesPerRow,
                rowsPerImage: texture._rowsPerImage,
                width: extent.width,
                height: extent.height,
                depthOrArrayLayers: extent.depthOrArrayLayers,
                mipLevel: destination.mipLevel || 0,
                origin: destination.origin || {}
            }));
        }
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
    }
    return picked;
}

function __createMockGPUDevice(adapter, descriptor, init) {
    // pulp #2101 — third `init` arg carries the bridge descriptor returned by
    // `__describeNativeDeviceImpl`. When `init.nativeBridge` is true, the
    // device is wired so `createTexture` mints a native Dawn texture handle
    // (via `__gpuCreateTextureImpl`) instead of a pure mock. Without this
    // branch every Three.js draw call lands in a no-op mock device and the
    // demo's WebGPU canvas stays solid black.
    descriptor = descriptor || {};
    init = init || {};
    var device = {
        _objectName: "GPUDevice",
        label: descriptor.label || "",
        _nativeBridge: !!init.nativeBridge,
        features: __createFeatureSet(__pickDeviceFeatures(adapter, descriptor)),
        limits: __mergeMockGpuLimits(descriptor.requiredLimits),
        queue: __createMockGPUQueue({ nativeBridge: !!init.nativeBridge }),
        adapterInfo: adapter && adapter.info ? adapter.info : null,
        lost: new Promise(function() {}),
        _errorScopes: [],
        _destroyed: false
    };
    device.createBuffer = function(bufferDescriptor) { return __createMockGPUBuffer(bufferDescriptor || {}); };
    device.createTexture = function(textureDescriptor) {
        textureDescriptor = textureDescriptor || {};
        var nativeTextureId = "";
        if (device._nativeBridge && typeof __gpuCreateTextureImpl === "function") {
            nativeTextureId = String(__gpuCreateTextureImpl(JSON.stringify({
                size: textureDescriptor.size || {},
                format: textureDescriptor.format || null,
                usage: textureDescriptor.usage || 0,
                mipLevelCount: textureDescriptor.mipLevelCount || 1,
                sampleCount: textureDescriptor.sampleCount || 1,
                label: textureDescriptor.label || ""
            })) || "");
        }
        var t = __createMockGPUTexture(textureDescriptor);
        if (nativeTextureId) {
            t._nativeBridge = true;
            t._nativeTextureId = nativeTextureId;
        }
        return t;
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
    // pulp #2101 — Three.js WebGPUPipelineUtils calls pushErrorScope/popErrorScope
    // around every pipeline create. The async-executor pattern swallows throws
    // when these are missing, so a `TypeError: device.pushErrorScope is not a
    // function` becomes a silently-empty mock canvas. Provide no-op error scopes
    // (we don't surface bridge errors back to JS yet).
    device.pushErrorScope = function(filter) { device._errorScopes.push({ filter: filter }); };
    device.popErrorScope = function() { device._errorScopes.pop(); return Promise.resolve(null); };
    device.addEventListener = function() {};
    device.removeEventListener = function() {};
    return device;
}

// pulp #2101 — bridge-aware adapter factory. When `init.nativeBridge` is true,
// `requestDevice` asks the host for a device descriptor (`__describeNativeDeviceImpl`)
// and forwards `init.nativeBridge` into `__createMockGPUDevice` so the device's
// `createTexture` mints real Dawn handles. The legacy `__createMockGPUAdapter`
// factory below stays for non-native paths and aliases this one.
function __createGPUAdapter(init) {
    init = init || {};
    var adapter = {
        _objectName: "GPUAdapter",
        name: init.name || "Pulp Native Adapter",
        backend: init.backend || __mockGpuInfo().backend,
        preferredCanvasFormat: init.preferredCanvasFormat || __mockPreferredCanvasFormat(),
        features: __createFeatureSet(init.features || [ "core-features-and-limits", "timestamp-query" ]),
        limits: __mergeMockGpuLimits(init.limits),
        info: init.info || { vendor: "Pulp", architecture: init.backend || __mockGpuInfo().backend, description: init.name || "Pulp Native Adapter" },
        _nativeBridge: !!init.nativeBridge
    };
    adapter.requestDevice = function(descriptor) {
        var deviceInit = {};
        if (adapter._nativeBridge && typeof __describeNativeDeviceImpl === "function") {
            deviceInit = __describeNativeDeviceImpl(descriptor || {}) || {};
            deviceInit.nativeBridge = true;
        }
        return Promise.resolve(__createMockGPUDevice(adapter, descriptor || {}, deviceInit));
    };
    return adapter;
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
