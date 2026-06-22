// ═══════════════════════════════════════════════════════════════════════════════
// Native GPU canvas helpers extracted from web-compat-canvas.js.
// ═══════════════════════════════════════════════════════════════════════════════
//
// Per the U-2 export map, the web-compat-canvas.js file (2,117 lines)
// splits naturally into four concerns:
//   1. Canvas2D core (~700 lines, stays in parent web-compat-canvas.js)
//   2. Matrix / transform (~400 lines, stays in parent for now)
//   3. **Native GPU helpers** (this file)
//   4. Path / text / image methods (stays in parent for now)
//
// This file owns the GPU-related shims that React + WebGPU consumers
// expect on window.pulp.gpu:
//   - __ensurePulpGpuHelpers() — install GPU helper functions if absent
//   - __installNativeGpuCommandAugmentation() — fan out GPU commands to
//     the native bridge via `__nativeGpuCommand__`
//   - __createGPUCanvasContext() — Canvas.getContext("webgpu") path
//   - __createMockGPUCanvasContext() — fallback for non-GPU builds
//   - _coerceCanvasDimension() — width/height parsing helper
//
// Embed order: this file is loaded AFTER web-compat-canvas.js so the
// CanvasRenderingContext2D constructor + window.pulp.gpu surface are
// already in scope when consumers later call getContext("webgpu").

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
            // Stamp the canvas id directly onto the texture so its `createView()`
            // propagates the bridge identity into the GPUTextureView attachment
            // that encoder.draw inspects.
            bridgedTexture._nativeCanvasId =
                (context.canvas && context.canvas._id) ? context.canvas._id : "";
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
