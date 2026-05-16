// ═══════════════════════════════════════════════════════════════════════════════
// Native GPU buffered draw augmentation
// ═══════════════════════════════════════════════════════════════════════════════

function __installNativeGpuBufferedDrawAugmentation() {
    if (typeof __createMockGPURenderPassEncoder !== "function" ||
        typeof __createMockGPUQueue !== "function" ||
        typeof __createMockGPUDevice !== "function") {
        return;
    }
    if (__installNativeGpuBufferedDrawAugmentation._installed) return;

    function cloneBufferBytes(binding) {
        if (!binding || !binding.buffer || !binding.buffer._bytes) return [];
        var source = binding.buffer._bytes;
        var begin = binding.offset == null ? 0 : binding.offset;
        var end = binding.size == null ? source.length : begin + binding.size;
        if (begin < 0) begin = 0;
        if (end < begin) end = begin;
        return Array.from(source.slice(begin, end));
    }

    function noteBufferedSkip(reason, details) {
        try {
            if (typeof globalThis !== "undefined") {
                if (!globalThis.__phase13BufferedSkips) {
                    globalThis.__phase13BufferedSkips = [];
                }
                globalThis.__phase13BufferedSkips.push(JSON.stringify({
                    reason: reason,
                    details: details || {}
                }));
            }
        } catch (_) {}
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
                    resource._nativeBridge && resource._nativeTextureId) {
                    serializedEntries.push({
                        binding: binding,
                        visibility: visibility,
                        resourceType: "textureView",
                        sourceTextureId: resource._nativeTextureId,
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

    function createBufferedDrawPayload(attachment, attachmentView, currentPipeline, currentBindGroups, currentVertexBuffers, currentIndexBuffer, drawDescriptor, depthStencil) {
        if (!attachmentView || !attachmentView._nativeBridge ||
            !currentPipeline || !currentPipeline._nativeBridge || !drawDescriptor) {
            noteBufferedSkip("missing-native-bridge", {
                attachmentNativeBridge: !!(attachmentView && attachmentView._nativeBridge),
                attachmentCanvasId: attachmentView && attachmentView._nativeCanvasId ? attachmentView._nativeCanvasId : "",
                attachmentTextureId: attachmentView && attachmentView._nativeTextureId ? attachmentView._nativeTextureId : "",
                pipelineNativeBridge: !!(currentPipeline && currentPipeline._nativeBridge)
            });
            return null;
        }

        var vertex = currentPipeline.vertex || {};
        var fragment = currentPipeline.fragment || {};
        var vertexModule = vertex.module || {};
        var fragmentModule = fragment.module || {};
        var vertexLayouts = vertex.buffers || [];
        var serializedVertexBuffers = [];
        var hasVertexBuffer = false;

        for (var slot = 0; slot < currentVertexBuffers.length; ++slot) {
            var binding = currentVertexBuffers[slot];
            if (!binding) continue;
            hasVertexBuffer = true;
            var layout = vertexLayouts[slot] || {};
            var attributes = layout.attributes || [];
            var serializedAttributes = [];
            for (var i = 0; i < attributes.length; ++i) {
                var attribute = attributes[i] || {};
                serializedAttributes.push({
                    shaderLocation: attribute.shaderLocation == null ? 0 : attribute.shaderLocation,
                    format: attribute.format || "float32x2",
                    offset: attribute.offset == null ? 0 : attribute.offset
                });
            }
            serializedVertexBuffers.push({
                slot: slot,
                arrayStride: layout.arrayStride == null ? 0 : layout.arrayStride,
                stepMode: layout.stepMode || "vertex",
                attributes: serializedAttributes,
                data: cloneBufferBytes(binding)
            });
        }

        if (!hasVertexBuffer) {
            noteBufferedSkip("missing-vertex-buffer", {
                attachmentCanvasId: attachmentView && attachmentView._nativeCanvasId ? attachmentView._nativeCanvasId : "",
                attachmentTextureId: attachmentView && attachmentView._nativeTextureId ? attachmentView._nativeTextureId : ""
            });
            return null;
        }

        var payload = {
            vertexCode: vertexModule.code || "",
            vertexEntryPoint: vertex.entryPoint || "main",
            fragmentCode: fragmentModule.code || "",
            fragmentEntryPoint: fragment.entryPoint || "main",
            format: attachmentView.format || (fragment.targets && fragment.targets[0] && fragment.targets[0].format) || __mockPreferredCanvasFormat(),
            topology: currentPipeline.primitive && currentPipeline.primitive.topology ? currentPipeline.primitive.topology : "triangle-list",
            vertexBuffers: serializedVertexBuffers,
            drawType: drawDescriptor.drawType || "draw"
        };
        if (attachment) {
            payload.loadOp = attachment.loadOp || "load";
            payload.storeOp = attachment.storeOp || "store";
            if (attachment.clearValue) {
                payload.clearValue = {
                    r: Number(attachment.clearValue.r == null ? 0 : attachment.clearValue.r),
                    g: Number(attachment.clearValue.g == null ? 0 : attachment.clearValue.g),
                    b: Number(attachment.clearValue.b == null ? 0 : attachment.clearValue.b),
                    a: Number(attachment.clearValue.a == null ? 1 : attachment.clearValue.a)
                };
            }
        }
        if (attachmentView._nativeCanvasId) {
            payload.canvasId = attachmentView._nativeCanvasId;
        } else if (attachmentView._nativeTextureId) {
            payload.targetTextureId = attachmentView._nativeTextureId;
        } else {
            noteBufferedSkip("missing-native-target", {
                attachmentNativeBridge: !!attachmentView._nativeBridge
            });
            return null;
        }

        var bindGroups = serializeBindGroups(currentBindGroups, vertexModule.code || "", fragmentModule.code || "");
        if (bindGroups) {
            payload.bindGroups = bindGroups;
        }

        if (currentIndexBuffer && currentIndexBuffer.buffer && currentIndexBuffer.buffer._bytes) {
            payload.indexBuffer = {
                format: currentIndexBuffer.format || "uint32",
                data: cloneBufferBytes(currentIndexBuffer)
            };
        }

        if (drawDescriptor.drawType === "draw-indexed") {
            payload.indexCount = drawDescriptor.indexCount == null ? 0 : drawDescriptor.indexCount;
            payload.instanceCount = drawDescriptor.instanceCount == null ? 1 : drawDescriptor.instanceCount;
            payload.firstIndex = drawDescriptor.firstIndex == null ? 0 : drawDescriptor.firstIndex;
            payload.baseVertex = drawDescriptor.baseVertex == null ? 0 : drawDescriptor.baseVertex;
            payload.firstInstance = drawDescriptor.firstInstance == null ? 0 : drawDescriptor.firstInstance;
        } else {
            payload.vertexCount = drawDescriptor.vertexCount == null ? 0 : drawDescriptor.vertexCount;
            payload.instanceCount = drawDescriptor.instanceCount == null ? 1 : drawDescriptor.instanceCount;
            payload.firstVertex = drawDescriptor.firstVertex == null ? 0 : drawDescriptor.firstVertex;
            payload.firstInstance = drawDescriptor.firstInstance == null ? 0 : drawDescriptor.firstInstance;
        }

        return payload;
    }

    function createPresentTexturePayload(attachmentView, currentPipeline, currentBindGroups) {
        if (!attachmentView || !attachmentView._nativeBridge || !attachmentView._nativeCanvasId) {
            return null;
        }

        if (!currentBindGroups || typeof currentBindGroups.length !== "number") {
            return null;
        }

        for (var groupIndex = 0; groupIndex < currentBindGroups.length; ++groupIndex) {
            var bindGroup = currentBindGroups[groupIndex];
            if (!bindGroup || !bindGroup.entries || typeof bindGroup.entries.length !== "number") continue;
            for (var i = 0; i < bindGroup.entries.length; ++i) {
                var entry = bindGroup.entries[i];
                var resource = entry && entry.resource ? entry.resource : null;
                if (resource && resource._objectName === "GPUTextureView" &&
                    resource._nativeBridge && resource._nativeTextureId) {
                    return {
                        canvasId: attachmentView._nativeCanvasId,
                        sourceTextureId: resource._nativeTextureId
                    };
                }
            }
        }

        return null;
    }

    var originalCreateMockGPURenderPassEncoder = __createMockGPURenderPassEncoder;
    var originalCreateMockGPUQueue = __createMockGPUQueue;
    var originalCreateMockGPUDevice = __createMockGPUDevice;

    __createMockGPURenderPassEncoder = function(init) {
        var encoder = originalCreateMockGPURenderPassEncoder(init || {});
        var currentPipeline = null;
        var currentBindGroups = [];
        var currentVertexBuffers = [];
        var currentIndexBuffer = null;
        var emittedBufferedDraw = false;
        var descriptor = init && init.descriptor ? init.descriptor : {};
        var attachments = descriptor.colorAttachments || [];
        var attachment = attachments.length > 0 ? attachments[0] : null;
        var attachmentView = attachment && attachment.view ? attachment.view : null;
        // depthStencil is referenced by encoder.draw / encoder.drawIndexed
        // when forwarding to createBufferedDrawPayload — declare it here so
        // those references resolve through closure (was a latent bug —
        // encoder.draw passed `depthStencil` without it being defined,
        // throwing ReferenceError once attachmentView._nativeBridge is true).
        var depthStencil = descriptor.depthStencilAttachment || null;
        var originalSetPipeline = encoder.setPipeline;
        var originalSetBindGroup = encoder.setBindGroup;
        var originalSetVertexBuffer = encoder.setVertexBuffer;
        var originalSetIndexBuffer = encoder.setIndexBuffer;
        var originalDraw = encoder.draw;
        var originalDrawIndexed = encoder.drawIndexed;
        var originalExecuteBundles = encoder.executeBundles;

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

        encoder.setVertexBuffer = function(slot, buffer, offset, size) {
            currentVertexBuffers[slot == null ? 0 : slot] = {
                buffer: buffer || null,
                offset: offset == null ? 0 : offset,
                size: size
            };
            if (typeof originalSetVertexBuffer === "function") {
                return originalSetVertexBuffer.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.setIndexBuffer = function(buffer, format, offset, size) {
            currentIndexBuffer = {
                buffer: buffer || null,
                format: format || "uint32",
                offset: offset == null ? 0 : offset,
                size: size
            };
            if (typeof originalSetIndexBuffer === "function") {
                return originalSetIndexBuffer.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.draw = function(vertexCount, instanceCount, firstVertex, firstInstance) {
            var bufferedPayload = createBufferedDrawPayload(attachment, attachmentView, currentPipeline, currentBindGroups, currentVertexBuffers, currentIndexBuffer, {
                drawType: "draw",
                vertexCount: vertexCount,
                instanceCount: instanceCount,
                firstVertex: firstVertex,
                firstInstance: firstInstance
            }, depthStencil);
            if (bufferedPayload && typeof init.onEnd === "function") {
                emittedBufferedDraw = true;
                init.onEnd({
                    type: "native-draw-current-texture-buffered",
                    payload: bufferedPayload
                });
                return;
            }
            if (typeof originalDraw === "function") {
                return originalDraw.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.drawIndexed = function(indexCount, instanceCount, firstIndex, baseVertex, firstInstance) {
            var bufferedPayload = createBufferedDrawPayload(attachment, attachmentView, currentPipeline, currentBindGroups, currentVertexBuffers, currentIndexBuffer, {
                drawType: "draw-indexed",
                indexCount: indexCount,
                instanceCount: instanceCount,
                firstIndex: firstIndex,
                baseVertex: baseVertex,
                firstInstance: firstInstance
            }, depthStencil);
            if (bufferedPayload && typeof init.onEnd === "function") {
                emittedBufferedDraw = true;
                init.onEnd({
                    type: "native-draw-current-texture-buffered",
                    payload: bufferedPayload
                });
                return;
            }
            if (typeof originalDrawIndexed === "function") {
                return originalDrawIndexed.apply(encoder, arguments);
            }
            return undefined;
        };

        encoder.executeBundles = function(bundles) {
            if (!bundles || typeof bundles.length !== "number") {
                if (typeof originalExecuteBundles === "function") {
                    return originalExecuteBundles.apply(encoder, arguments);
                }
                return;
            }
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
                    } else if (command.type === "set-vertex-buffer") {
                        encoder.setVertexBuffer(command.slot, command.buffer, command.offset, command.size);
                    } else if (command.type === "set-index-buffer") {
                        encoder.setIndexBuffer(command.buffer, command.format, command.offset, command.size);
                    } else if (command.type === "draw") {
                        encoder.draw(command.vertexCount, command.instanceCount, command.firstVertex, command.firstInstance);
                    } else if (command.type === "draw-indexed") {
                        encoder.drawIndexed(command.indexCount, command.instanceCount, command.firstIndex, command.baseVertex, command.firstInstance);
                    }
                }
            }
        };

        var originalEnd = encoder.end;
        encoder.end = function() {
            if (!emittedBufferedDraw && typeof init.onEnd === "function") {
                var presentPayload = createPresentTexturePayload(attachmentView, currentPipeline, currentBindGroups);
                if (presentPayload) {
                    init.onEnd({
                        type: "native-present-texture-buffered",
                        payload: presentPayload
                    });
                }
            }
            if (typeof originalEnd === "function") {
                return originalEnd.apply(encoder, arguments);
            }
            return undefined;
        };

        return encoder;
    };

    __createMockGPUQueue = function(init) {
        var queue = originalCreateMockGPUQueue(init || {});
        queue.submit = function(commandBuffers) {
            queue._submitCount += commandBuffers && typeof commandBuffers.length === "number" ? commandBuffers.length : 0;
            if (!queue._nativeBridge || !commandBuffers) {
                return;
            }
            for (var i = 0; i < commandBuffers.length; ++i) {
                var commandBuffer = commandBuffers[i];
                var commands = commandBuffer && commandBuffer._commands ? commandBuffer._commands : [];
                for (var j = 0; j < commands.length; ++j) {
                    var command = commands[j];
                    if (!command) continue;
                    if (command.type === "native-clear-current-texture" && typeof __gpuQueueSubmitImpl === "function") {
                        __gpuQueueSubmitImpl(command.canvasId, command.r, command.g, command.b, command.a);
                        continue;
                    }
                    if (command.type === "native-draw-current-texture-buffered" &&
                        typeof __gpuQueueDrawBufferedImpl === "function") {
                        __gpuQueueDrawBufferedImpl(command.payload);
                        continue;
                    }
                    if (command.type === "native-present-texture-buffered" &&
                        typeof __gpuQueuePresentTextureImpl === "function") {
                        __gpuQueuePresentTextureImpl(command.payload);
                    }
                }
            }
        };
        return queue;
    };

    __createMockGPUDevice = function(adapter, descriptor, init) {
        var device = originalCreateMockGPUDevice(adapter, descriptor, init || {});
        device.createRenderBundleEncoder = function(bundleDescriptor) {
            var commands = [];
            return {
                _objectName: "GPURenderBundleEncoder",
                label: bundleDescriptor && bundleDescriptor.label ? bundleDescriptor.label : "",
                setPipeline: function(pipeline) {
                    commands.push({ type: "set-pipeline", pipeline: pipeline || null });
                },
                setBindGroup: function(index, bindGroup) {
                    commands.push({ type: "set-bind-group", index: index == null ? 0 : index, bindGroup: bindGroup || null });
                },
                setVertexBuffer: function(slot, buffer, offset, size) {
                    commands.push({
                        type: "set-vertex-buffer",
                        slot: slot == null ? 0 : slot,
                        buffer: buffer || null,
                        offset: offset == null ? 0 : offset,
                        size: size
                    });
                },
                setIndexBuffer: function(buffer, format, offset, size) {
                    commands.push({
                        type: "set-index-buffer",
                        buffer: buffer || null,
                        format: format || "uint32",
                        offset: offset == null ? 0 : offset,
                        size: size
                    });
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
                drawIndexed: function(indexCount, instanceCount, firstIndex, baseVertex, firstInstance) {
                    commands.push({
                        type: "draw-indexed",
                        indexCount: indexCount == null ? 0 : indexCount,
                        instanceCount: instanceCount == null ? 1 : instanceCount,
                        firstIndex: firstIndex == null ? 0 : firstIndex,
                        baseVertex: baseVertex == null ? 0 : baseVertex,
                        firstInstance: firstInstance == null ? 0 : firstInstance
                    });
                },
                finish: function(finishDescriptor) {
                    return {
                        _objectName: "GPURenderBundle",
                        label: finishDescriptor && finishDescriptor.label ? finishDescriptor.label : "",
                        _commands: commands.slice()
                    };
                }
            };
        };
        return device;
    };

    __installNativeGpuBufferedDrawAugmentation._installed = true;
}

if (typeof __ensurePulpGpuHelpers === "function") {
    var __originalEnsurePulpGpuHelpers = __ensurePulpGpuHelpers;
    __ensurePulpGpuHelpers = function() {
        __originalEnsurePulpGpuHelpers();
        __installNativeGpuBufferedDrawAugmentation._installed = false;
        __installNativeGpuBufferedDrawAugmentation();
    };
}

__installNativeGpuBufferedDrawAugmentation();
