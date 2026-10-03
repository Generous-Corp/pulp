// Matched, test-only native Metal controls for the shared GPU-audio multiply.
//
// This is deliberately outside GpuAudioTransport and never runs from an audio
// callback. Both modes use the same no-copy buffers, pipeline, two-slot flight
// depth, and semaphore completion service. It measures submission and
// completion phases so a future Dawn receipt can be compared without mixing
// observer polling or per-trial resource construction into the result.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#if __has_include("detail/dawn_shared_io_provider.hpp")
#include "detail/dawn_shared_io_provider.hpp"
#include "detail/shared_io_compute_plan.hpp"
#define PULP_HAS_DAWN_COMPARATOR 1
#else
#define PULP_HAS_DAWN_COMPARATOR 0
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if __has_include(<Metal/MTL4CommandQueue.h>) && \
    defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
#define PULP_HAS_METAL4_COMPARATOR 1
#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTL4CommandQueue.h>
#else
#define PULP_HAS_METAL4_COMPARATOR 0
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kFrames = 32;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kComplexValues = kFrames * kChannels;
constexpr std::uint32_t kFloatValues = kComplexValues * 2;
constexpr std::uint32_t kTrials = 128;
constexpr std::uint32_t kWarmup = 8;
constexpr std::uint32_t kFlightDepth = 2;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

struct AlignedBytes {
    void* pointer = nullptr;
    explicit AlignedBytes(std::size_t bytes) {
        if (posix_memalign(&pointer, 4096, bytes) == 0)
            std::memset(pointer, 0, bytes);
    }
    ~AlignedBytes() {
        std::free(pointer);
    }
    AlignedBytes(const AlignedBytes&) = delete;
    AlignedBytes& operator=(const AlignedBytes&) = delete;
};

struct Completion {
    dispatch_semaphore_t semaphore = nullptr;
    std::mutex mutex;
    bool observed = false;
    bool error = false;
    double gpu_start_ns = 0.0;
    double gpu_end_ns = 0.0;
    Completion() : semaphore(dispatch_semaphore_create(0)) {}
    ~Completion() {
        if (semaphore)
            dispatch_release(semaphore);
    }
};

struct Slot {
    AlignedBytes input{kFloatValues * sizeof(float)};
    AlignedBytes output{kFloatValues * sizeof(float)};
    id<MTLBuffer> input_buffer = nil;
    id<MTLBuffer> output_buffer = nil;
    Completion completion;
};

struct Record {
    std::uint64_t sequence = 0;
    std::uint64_t encode_start_ns = 0;
    std::uint64_t encode_end_ns = 0;
    std::uint64_t commit_start_ns = 0;
    std::uint64_t commit_return_ns = 0;
    std::uint64_t completion_observed_ns = 0;
    double gpu_start_ns = 0.0;
    double gpu_end_ns = 0.0;
    bool oracle = false;
};

struct Result {
    const char* mode = "unknown";
    std::vector<Record> records;
    bool unavailable = false;
    bool gpu_timestamps_available = true;
    std::string reason;
};

id<MTLBuffer> no_copy_buffer(id<MTLDevice> device, AlignedBytes& storage, std::size_t bytes) {
    if (!storage.pointer)
        return nil;
    return [device newBufferWithBytesNoCopy:storage.pointer
                                     length:bytes
                                    options:MTLResourceStorageModeShared
                                deallocator:nil];
}

id<MTLLibrary> make_library(id<MTLDevice> device, NSError** error) {
    NSString* source = @""
                        "#include <metal_stdlib>\n"
                        "using namespace metal;\n"
                        "kernel void complex_multiply(const device float* a [[buffer(0)]],\n"
                        "                              device float* out [[buffer(1)]],\n"
                        "                              uint id [[thread_position_in_grid]]) {\n"
                        "  if (id >= 128u) return; uint p = id & ~1u;\n"
                        "  float ar = a[p], ai = a[p + 1u];\n"
                        "  out[p] = ar * 1.75f - ai * 0.25f;\n"
                        "  out[p + 1u] = ar * 0.25f + ai * 1.75f;\n"
                        "}\n";
    return [device newLibraryWithSource:source options:nil error:error];
}

bool validate(Slot& slot, std::uint64_t sequence) {
    const auto* in = static_cast<const float*>(slot.input.pointer);
    const auto* out = static_cast<const float*>(slot.output.pointer);
    for (std::uint32_t i = 0; i < kFloatValues; i += 2) {
        const float er = in[i] * 1.75f - in[i + 1] * 0.25f;
        const float ei = in[i] * 0.25f + in[i + 1] * 1.75f;
        if (!std::isfinite(out[i]) || std::fabs(out[i] - er) > 1.0e-5f ||
            !std::isfinite(out[i + 1]) || std::fabs(out[i + 1] - ei) > 1.0e-5f)
            return false;
    }
    (void)sequence;
    return true;
}

void reset(Slot& slot, std::uint64_t sequence) {
    auto* in = static_cast<float*>(slot.input.pointer);
    auto* out = static_cast<float*>(slot.output.pointer);
    for (std::uint32_t i = 0; i < kFloatValues; ++i)
        in[i] = static_cast<float>((sequence % 97u) * 0.01 + i * 0.003 - 0.2);
    std::fill(out, out + kFloatValues, 0.0f);
    std::lock_guard lock(slot.completion.mutex);
    slot.completion.observed = false;
    slot.completion.error = false;
    slot.completion.gpu_start_ns = 0.0;
    slot.completion.gpu_end_ns = 0.0;
}

void wait_completion(Slot& slot, Record& record) {
    dispatch_semaphore_wait(slot.completion.semaphore, DISPATCH_TIME_FOREVER);
    {
        std::lock_guard lock(slot.completion.mutex);
        record.gpu_start_ns = slot.completion.gpu_start_ns;
        record.gpu_end_ns = slot.completion.gpu_end_ns;
        record.oracle = !slot.completion.error;
    }
    record.completion_observed_ns = now_ns();
}

Result run_ordinary(id<MTLDevice> device, id<MTLComputePipelineState> pipeline) {
    Result result{.mode = "ordinary_metal"};
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
        result.unavailable = true;
        result.reason = "queue_unavailable";
        return result;
    }
    std::array<Slot, kFlightDepth> slots;
    for (auto& slot : slots) {
        slot.input_buffer = no_copy_buffer(device, slot.input, kFloatValues * sizeof(float));
        slot.output_buffer = no_copy_buffer(device, slot.output, kFloatValues * sizeof(float));
        if (!slot.input_buffer || !slot.output_buffer) {
            result.unavailable = true;
            result.reason = "shared_buffer_unavailable";
            return result;
        }
    }
    const auto width = std::max<NSUInteger>(1, pipeline.threadExecutionWidth);
    result.records.reserve(kTrials);
    std::array<Record, kFlightDepth> pending{};
    for (std::uint32_t sequence = 0; sequence < kWarmup + kTrials; ++sequence) {
        const auto index = sequence % kFlightDepth;
        auto& slot = slots[index];
        if (sequence >= kFlightDepth) {
            Record retired = pending[(sequence - kFlightDepth) % kFlightDepth];
            wait_completion(slots[(sequence - kFlightDepth) % kFlightDepth], retired);
            if (!validate(slots[(sequence - kFlightDepth) % kFlightDepth], retired.sequence))
                retired.oracle = false;
            if (retired.sequence >= kWarmup)
                result.records.push_back(retired);
        }
        reset(slot, sequence);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (!command) {
            result.unavailable = true;
            result.reason = "command_buffer_unavailable";
            return result;
        }
        Record record{};
        record.sequence = sequence;
        record.encode_start_ns = now_ns();
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:slot.input_buffer offset:0 atIndex:0];
        [encoder setBuffer:slot.output_buffer offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(kFloatValues, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        record.encode_end_ns = now_ns();
        Slot* slot_ptr = &slot;
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
          Slot* target = slot_ptr;
          std::lock_guard lock(target->completion.mutex);
          target->completion.error = completed.status != MTLCommandBufferStatusCompleted;
          target->completion.gpu_start_ns = completed.GPUStartTime * 1.0e9;
          target->completion.gpu_end_ns = completed.GPUEndTime * 1.0e9;
          target->completion.observed = true;
          dispatch_semaphore_signal(target->completion.semaphore);
        }];
        record.commit_start_ns = now_ns();
        [command commit];
        record.commit_return_ns = now_ns();
        pending[index] = record;
    }
    for (std::uint32_t i = 0; i < kFlightDepth; ++i) {
        Record retired = pending[(kWarmup + kTrials - kFlightDepth + i) % kFlightDepth];
        wait_completion(slots[(kWarmup + kTrials - kFlightDepth + i) % kFlightDepth], retired);
        retired.oracle = validate(slots[(kWarmup + kTrials - kFlightDepth + i) % kFlightDepth],
                                  retired.sequence);
        result.records.push_back(retired);
    }
    return result;
}

#if PULP_HAS_METAL4_COMPARATOR
Result run_metal4(id<MTLDevice> device, id<MTLComputePipelineState> pipeline)
    API_AVAILABLE(macos(26.0)) {
    Result result{.mode = "metal4"};
    if (!@available(macOS 26.0, *)) {
        result.unavailable = true;
        result.reason = "os_before_macos26";
        return result;
    }
    id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
    if (!queue) {
        result.unavailable = true;
        result.reason = "queue_unavailable";
        return result;
    }
    std::array<Slot, kFlightDepth> slots;
    std::array<id<MTL4CommandAllocator>, kFlightDepth> allocators{};
    std::array<id<MTL4ArgumentTable>, kFlightDepth> tables{};
    MTLResidencySetDescriptor* residency_descriptor = [MTLResidencySetDescriptor new];
    residency_descriptor.initialCapacity = kFlightDepth * 2;
    NSError* residency_error = nil;
    id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:residency_descriptor
                                                                    error:&residency_error];
    if (!residency) {
        result.unavailable = true;
        result.reason = "residency_set_unavailable";
        return result;
    }
    for (std::uint32_t i = 0; i < kFlightDepth; ++i) {
        auto& slot = slots[i];
        slot.input_buffer = no_copy_buffer(device, slot.input, kFloatValues * sizeof(float));
        slot.output_buffer = no_copy_buffer(device, slot.output, kFloatValues * sizeof(float));
        allocators[i] = [device newCommandAllocator];
        MTL4ArgumentTableDescriptor* descriptor = [MTL4ArgumentTableDescriptor new];
        descriptor.maxBufferBindCount = 2;
        descriptor.initializeBindings = YES;
        NSError* error = nil;
        tables[i] = [device newArgumentTableWithDescriptor:descriptor error:&error];
        if (!slot.input_buffer || !slot.output_buffer || !allocators[i] || !tables[i]) {
            result.unavailable = true;
            result.reason = "persistent_resource_unavailable";
            return result;
        }
        [tables[i] setAddress:slot.input_buffer.gpuAddress atIndex:0];
        [tables[i] setAddress:slot.output_buffer.gpuAddress atIndex:1];
        const id<MTLAllocation> allocations[] = {slot.input_buffer, slot.output_buffer};
        [residency addAllocations:allocations count:2];
    }
    [residency commit];
    [residency requestResidency];
    [queue addResidencySet:residency];
    const auto width = std::max<NSUInteger>(1, pipeline.threadExecutionWidth);
    result.records.reserve(kTrials);
    std::array<Record, kFlightDepth> pending{};
    for (std::uint32_t sequence = 0; sequence < kWarmup + kTrials; ++sequence) {
        const auto index = sequence % kFlightDepth;
        if (sequence >= kFlightDepth) {
            Record retired = pending[(sequence - kFlightDepth) % kFlightDepth];
            wait_completion(slots[(sequence - kFlightDepth) % kFlightDepth], retired);
            retired.oracle =
                validate(slots[(sequence - kFlightDepth) % kFlightDepth], retired.sequence);
            if (retired.sequence >= kWarmup)
                result.records.push_back(retired);
        }
        auto& slot = slots[index];
        reset(slot, sequence);
        id<MTL4CommandBuffer> command = [device newCommandBuffer];
        if (!command) {
            result.unavailable = true;
            result.reason = "command_buffer_unavailable";
            return result;
        }
        Record record{};
        record.sequence = sequence;
        record.encode_start_ns = now_ns();
        [command beginCommandBufferWithAllocator:allocators[index]];
        [command useResidencySet:residency];
        id<MTL4ComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setArgumentTable:tables[index]];
        [encoder dispatchThreads:MTLSizeMake(kFloatValues, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        [command endCommandBuffer];
        record.encode_end_ns = now_ns();
        MTL4CommitOptions* options = [MTL4CommitOptions new];
        Slot* slot_ptr = &slot;
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
          Slot* target = slot_ptr;
          std::lock_guard lock(target->completion.mutex);
          target->completion.error = feedback.error != nil;
          target->completion.gpu_start_ns = feedback.GPUStartTime * 1.0e9;
          target->completion.gpu_end_ns = feedback.GPUEndTime * 1.0e9;
          target->completion.observed = true;
          dispatch_semaphore_signal(target->completion.semaphore);
        }];
        const id<MTL4CommandBuffer> buffers[] = {command};
        record.commit_start_ns = now_ns();
        [queue commit:buffers count:1 options:options];
        record.commit_return_ns = now_ns();
        pending[index] = record;
    }
    for (std::uint32_t i = 0; i < kFlightDepth; ++i) {
        const auto sequence = kWarmup + kTrials - kFlightDepth + i;
        Record retired = pending[sequence % kFlightDepth];
        wait_completion(slots[sequence % kFlightDepth], retired);
        retired.oracle = validate(slots[sequence % kFlightDepth], sequence);
        result.records.push_back(retired);
    }
    return result;
}
#endif

#if PULP_HAS_DAWN_COMPARATOR
Result run_dawn() {
    using pulp::gpu_audio::detail::DawnSharedIoProvider;
    using pulp::gpu_audio::detail::SharedIoArena;
    using pulp::gpu_audio::detail::SharedIoComputePlan;
    Result result{.mode = "dawn_wait_any", .gpu_timestamps_available = false};
    DawnSharedIoProvider::Options options;
    options.completion_policy = DawnSharedIoProvider::CompletionPolicy::WaitAny;
    auto created = DawnSharedIoProvider::create(options);
    if (!created.provider) {
        result.unavailable = true;
        result.reason = created.reason;
        return result;
    }

    // A one-sample impulse makes the prepared FFT convolution an identity
    // transform while retaining the production shared-I/O program path.
    constexpr std::uint32_t fft_size = kFrames * 2;
    std::vector<float> normalized_spectrum(static_cast<std::size_t>(fft_size) * 2, 0.0f);
    for (std::uint32_t bin = 0; bin < fft_size; ++bin)
        normalized_spectrum[static_cast<std::size_t>(bin) * 2] = 1.0f / fft_size;
    const auto bytes = static_cast<std::size_t>(kChannels) * fft_size * 2 * sizeof(float);
    auto program =
        created.provider->make_convolution_program({.fft_size = fft_size,
                                                    .channels = kChannels,
                                                    .logical_frames = kFrames,
                                                    .ir_length = 1,
                                                    .normalized_ir_spectrum = normalized_spectrum});
    SharedIoComputePlan plan;
    if (!program ||
        !plan.prepare(
            *created.provider,
            {.slots = kFlightDepth, .input_bytes_per_slot = bytes, .output_bytes_per_slot = bytes},
            std::move(program))) {
        result.unavailable = true;
        result.reason = "prepare_failed";
        return result;
    }

    std::array<Record, kFlightDepth> pending{};
    std::array<std::array<float, kChannels * kFrames>, kFlightDepth> expected{};
    result.records.reserve(kTrials);
    auto settle = [&](std::uint64_t sequence) {
        std::optional<SharedIoComputePlan::Completion> completion;
        while (!completion) {
            created.provider->service_until(now_ns() + 1'000'000);
            plan.drain(now_ns());
            while (auto candidate = plan.pop_completion()) {
                if (candidate->token.slot.stream_sequence == sequence + 1) {
                    completion = *candidate;
                    break;
                }
            }
        }
        auto record = pending[sequence % kFlightDepth];
        record.completion_observed_ns = now_ns();
        auto output = plan.acquire_output(*completion);
        record.oracle = output.has_value() &&
                        completion->status == SharedIoArena::CompletionStatus::RetiredSuccess;
        if (output) {
            const auto* values = reinterpret_cast<const float*>(output->bytes.data());
            for (std::uint32_t channel = 0; record.oracle && channel < kChannels; ++channel)
                for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
                    const auto offset =
                        static_cast<std::size_t>(channel) * fft_size * 2 + frame * 2;
                    const auto expected_value =
                        expected[sequence % kFlightDepth][channel * kFrames + frame];
                    if (!std::isfinite(values[offset]) ||
                        std::fabs(values[offset] - expected_value) > 1.0e-4f)
                        record.oracle = false;
                }
            const auto token = output->token;
            output.reset();
            if (!plan.release_output({token}))
                record.oracle = false;
        }
        if (record.sequence >= kWarmup)
            result.records.push_back(record);
    };

    for (std::uint32_t sequence = 0; sequence < kWarmup + kTrials; ++sequence) {
        if (sequence >= kFlightDepth)
            settle(sequence - kFlightDepth);
        const auto index = sequence % kFlightDepth;
        auto write = plan.acquire_input(sequence + 1, 0);
        if (!write) {
            result.unavailable = true;
            result.reason = "acquire_failed";
            return result;
        }
        auto* values = reinterpret_cast<float*>(write->bytes.data());
        for (std::uint32_t channel = 0; channel < kChannels; ++channel)
            for (std::uint32_t frame = 0; frame < fft_size; ++frame) {
                const float sample =
                    frame < kFrames ? static_cast<float>((sequence % 97u) * 0.01 +
                                                         (channel * kFrames + frame) * 0.003 - 0.2)
                                    : 0.0f;
                values[(static_cast<std::size_t>(channel) * fft_size + frame) * 2] = sample;
                values[(static_cast<std::size_t>(channel) * fft_size + frame) * 2 + 1] = 0.0f;
                if (frame < kFrames)
                    expected[index][channel * kFrames + frame] = sample;
            }
        Record record{};
        record.sequence = sequence;
        record.encode_start_ns = now_ns();
        record.encode_end_ns = now_ns();
        record.commit_start_ns = now_ns();
        if (!plan.submit({write->token, 0})) {
            result.unavailable = true;
            result.reason = "submit_failed";
            return result;
        }
        record.commit_return_ns = now_ns();
        pending[index] = record;
    }
    for (std::uint32_t i = 0; i < kFlightDepth; ++i)
        settle(kWarmup + kTrials - kFlightDepth + i);
    if (!plan.release()) {
        result.unavailable = true;
        result.reason = "release_failed";
    }
    return result;
}
#endif

void emit(const Result& result, id<MTLDevice> device) {
    std::printf("{\"schema\":\"pulp.gpu-audio.native-metal-matched.v1\",\"status\":\"%s\",\"mode\":"
                "\"%s\",\"device\":\"%s\",\"frames\":%u,\"channels\":%u,\"flight_depth\":%u,"
                "\"trials\":%zu,\"reason\":\"%s\",\"cpu_timestamp_domain\":\"steady_clock_ns\","
                "\"gpu_timestamp_domain\":\"%s\",\"records\":[",
                result.unavailable ? "unavailable" : "completed", result.mode,
                device.name.UTF8String, kFrames, kChannels, kFlightDepth, result.records.size(),
                result.reason.c_str(),
                result.gpu_timestamps_available ? "Metal_host_seconds_converted_to_ns"
                                                : "unavailable_for_Dawn_provider");
    for (std::size_t i = 0; i < result.records.size(); ++i) {
        if (i)
            std::printf(",");
        const auto& r = result.records[i];
        std::printf("{\"sequence\":%llu,\"encode_start_ns\":%llu,\"encode_end_ns\":%llu,\"commit_"
                    "start_ns\":%llu,\"commit_return_ns\":%llu,\"completion_observed_ns\":%llu,"
                    "\"gpu_start_ns\":%.0f,\"gpu_end_ns\":%.0f,\"oracle\":%s}",
                    (unsigned long long)r.sequence, (unsigned long long)r.encode_start_ns,
                    (unsigned long long)r.encode_end_ns, (unsigned long long)r.commit_start_ns,
                    (unsigned long long)r.commit_return_ns,
                    (unsigned long long)r.completion_observed_ns, r.gpu_start_ns, r.gpu_end_ns,
                    r.oracle ? "true" : "false");
    }
    std::printf("]}\n");
}

} // namespace

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
            return 77;
        NSError* error = nil;
        id<MTLLibrary> library = make_library(device, &error);
        id<MTLFunction> function =
            library ? [library newFunctionWithName:@"complex_multiply"] : nil;
        id<MTLComputePipelineState> pipeline =
            function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!pipeline)
            return 1;
        const auto ordinary = run_ordinary(device, pipeline);
        emit(ordinary, device);
        if (ordinary.unavailable)
            return 77;
        bool metal4_ok = false;
#if PULP_HAS_METAL4_COMPARATOR
        if (@available(macOS 26.0, *)) {
            const auto metal4 = run_metal4(device, pipeline);
            emit(metal4, device);
            if (metal4.unavailable)
                return 77;
            metal4_ok = std::all_of(metal4.records.begin(), metal4.records.end(),
                                    [](const Record& record) { return record.oracle; });
        } else {
            return 77;
        }
#else
        return 77;
#endif
        for (const auto& record : ordinary.records)
            if (!record.oracle)
                return 1;
        if (!metal4_ok)
            return 1;
#if PULP_HAS_DAWN_COMPARATOR
        const auto dawn = run_dawn();
        emit(dawn, device);
        if (dawn.unavailable)
            return 77;
        if (!std::all_of(dawn.records.begin(), dawn.records.end(),
                         [](const Record& record) { return record.oracle; }))
            return 1;
#else
        return 77;
#endif
        return 0;
    }
}
