// Native Metal availability/timing probe.
// This is deliberately separate from Dawn GpuCompute: it establishes whether
// a direct Metal compute seam exists on the host before any production wiring.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::printf("{\"verdict\":\"unavailable\",\"reason\":\"no_metal_device\"}\n");
            return 77;
        }

        // Metal 4 is reported as an observation only. The SDK/runtime check is
        // intentionally dynamic so older Xcode/OS SDKs still compile and run.
        const bool metal4_sdk =
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && (__MAC_OS_X_VERSION_MAX_ALLOWED >= 260000)
            true;
#else
            false;
#endif
        const bool metal4_runtime = NSClassFromString(@"MTL4CommandQueue") != nil;

        NSError* error = nil;
        NSString* source = @""
                            "#include <metal_stdlib>\n"
                            "using namespace metal;\n"
                            "kernel void add_one(const device float* input [[buffer(0)]],\n"
                            "                     device float* output [[buffer(1)]],\n"
                            "                     uint id [[thread_position_in_grid]]) {\n"
                            "    output[id] = input[id] + 1.0f;\n"
                            "}\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) {
            std::fprintf(stderr, "native Metal library compile failed: %s\n",
                         error.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"add_one"];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                                     error:&error];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!function || !pipeline || !queue) {
            std::fprintf(stderr, "native Metal pipeline/queue creation failed: %s\n",
                         error ? error.localizedDescription.UTF8String : "unknown");
            return 1;
        }

        constexpr NSUInteger count = 1024;
        std::vector<float> input(count, 0.25f);
        id<MTLBuffer> input_buffer = [device newBufferWithBytes:input.data()
                                                         length:input.size() * sizeof(float)
                                                        options:MTLResourceStorageModeShared];
        id<MTLBuffer> output_buffer = [device newBufferWithLength:input.size() * sizeof(float)
                                                          options:MTLResourceStorageModeShared];
        if (!input_buffer || !output_buffer)
            return 1;

        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input_buffer offset:0 atIndex:0];
        [encoder setBuffer:output_buffer offset:0 atIndex:1];
        const NSUInteger width = pipeline.threadExecutionWidth;
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];

        const auto start = std::chrono::steady_clock::now();
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            std::fprintf(stderr, "native Metal command failed: %s\n",
                         command_buffer.error.localizedDescription.UTF8String);
            return 1;
        }

        const float* output = static_cast<const float*>(output_buffer.contents);
        for (NSUInteger i = 0; i < count; ++i) {
            if (std::fabs(output[i] - 1.25f) > 1e-6f)
                return 1;
        }
        std::printf("{\"verdict\":\"passed\",\"device\":\"%s\","
                    "\"metal4_sdk\":%s,\"metal4_runtime\":%s,"
                    "\"dispatch_us\":%.3f,\"elements\":%lu}\n",
                    device.name.UTF8String, metal4_sdk ? "true" : "false",
                    metal4_runtime ? "true" : "false", elapsed, static_cast<unsigned long>(count));
        return 0;
    }
}
