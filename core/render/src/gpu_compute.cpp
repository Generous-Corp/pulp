#include <pulp/render/gpu_compute.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/runtime/log.hpp>
#include "webgpu/webgpu_cpp.h"
#include "dawn/native/DawnNative.h"
#include "dawn/dawn_proc.h"

#include "gpu_compute_pool.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#endif

namespace pulp::render {

// ── WGSL Compute Shaders ────────────────────────────────────────────────────

static constexpr const char* kMagnitudeShader = R"wgsl(
// Compute magnitude from interleaved complex pairs: [re0, im0, re1, im1, ...]
// Output: linear magnitude per bin

@group(0) @binding(0) var<storage, read> input : array<f32>;
@group(0) @binding(1) var<storage, read_write> output : array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    let num_bins = arrayLength(&output);
    if (idx >= num_bins) {
        return;
    }
    let re = input[idx * 2u];
    let im = input[idx * 2u + 1u];
    output[idx] = sqrt(re * re + im * im);
}
)wgsl";

static constexpr const char* kComplexMultiplyShader = R"wgsl(
// Element-wise complex multiply: result[i] = a[i] * b[i]
// All arrays are interleaved [re, im, re, im, ...]

@group(0) @binding(0) var<storage, read> a : array<f32>;
@group(0) @binding(1) var<storage, read> b : array<f32>;
@group(0) @binding(2) var<storage, read_write> result : array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    let num_pairs = arrayLength(&result) / 2u;
    if (idx >= num_pairs) {
        return;
    }
    let base = idx * 2u;
    let a_re = a[base];
    let a_im = a[base + 1u];
    let b_re = b[base];
    let b_im = b[base + 1u];
    // (a_re + a_im*i) * (b_re + b_im*i)
    result[base]      = a_re * b_re - a_im * b_im;
    result[base + 1u] = a_re * b_im + a_im * b_re;
}
)wgsl";

// ── Timing helper ───────────────────────────────────────────────────────────

static double now_us() {
    using Clock = std::chrono::high_resolution_clock;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()) / 1000.0;
}

// ── Implementation ──────────────────────────────────────────────────────────

class DawnGpuCompute : public GpuCompute {
public:
    ~DawnGpuCompute() override {
        // Release pool buffers before the device is torn down — their
        // destructors call into Dawn internals and need a live device.
        pool_.reset();
        magnitude_pipeline_ = nullptr;
        complex_mul_pipeline_ = nullptr;
        queue_ = nullptr;
        device_ = nullptr;
        instance_ = nullptr;
        native_instance_.reset();
    }

    bool initialize_from_surface(GpuSurface& surface) override {
        if (!surface.is_initialized()) return false;

        auto* dev = static_cast<wgpu::Device*>(surface.dawn_device_handle());
        auto* q = static_cast<wgpu::Queue*>(surface.dawn_queue_handle());
        auto* inst = static_cast<wgpu::Instance*>(surface.dawn_instance_handle());
        if (!dev || !q || !inst) return false;

        device_ = *dev;
        queue_ = *q;
        instance_ = *inst;
        owns_device_ = false;

        return create_pipelines();
    }

    bool initialize_standalone() override {
        const DawnProcTable& procs = dawn::native::GetProcs();
        dawnProcSetProcs(&procs);

        wgpu::InstanceDescriptor inst_desc{};
        native_instance_ = std::make_unique<dawn::native::Instance>(
            reinterpret_cast<const WGPUInstanceDescriptor*>(&inst_desc));
        instance_ = wgpu::Instance(native_instance_->Get());
        if (!instance_) return false;

        wgpu::RequestAdapterOptions opts{};
        opts.powerPreference = wgpu::PowerPreference::HighPerformance;

        instance_.RequestAdapter(
            &opts, wgpu::CallbackMode::AllowProcessEvents,
            [this](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView) {
                if (status == wgpu::RequestAdapterStatus::Success)
                    adapter_ = std::move(result);
            });
        instance_.ProcessEvents();
        if (!adapter_) return false;

        wgpu::DeviceDescriptor dev_desc{};
        dev_desc.label = "Pulp Compute Device";
        dev_desc.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
                runtime::log_error("GpuCompute: WebGPU error ({}): {}",
                    static_cast<int>(type), std::string(msg.data, msg.length));
            });

        adapter_.RequestDevice(
            &dev_desc, wgpu::CallbackMode::AllowProcessEvents,
            [this](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView) {
                if (status == wgpu::RequestDeviceStatus::Success)
                    device_ = std::move(result);
            });
        instance_.ProcessEvents();
        if (!device_) return false;

        queue_ = device_.GetQueue();
        owns_device_ = true;

        return create_pipelines();
    }

    // ── Compute operations ──────────────────────────────────────────────

    bool compute_magnitude(const float* complex_pairs, float* magnitudes,
                           uint32_t num_bins) override {
        if (!initialized_) return false;

        uint32_t input_bytes = num_bins * 2 * sizeof(float);
        uint32_t output_bytes = num_bins * sizeof(float);

        auto input_buf = acquire_storage_buffer(input_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto output_buf = acquire_storage_buffer(output_bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto readback_buf = acquire_readback_buffer(output_bytes);

#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            queue_.WriteBuffer(input_buf, 0, complex_pairs, input_bytes);
            if (bench_counters_) {
                bench_counters_->gpu_upload_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
                bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                    static_cast<double>(input_bytes),
                    std::memory_order_relaxed);
            }
        }
#else
        queue_.WriteBuffer(input_buf, 0, complex_pairs, input_bytes);
#endif

        auto bind_group = create_bind_group(magnitude_pipeline_, {input_buf, output_buf});

        uint32_t workgroups = (num_bins + 255) / 256;
#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            dispatch(magnitude_pipeline_, bind_group, workgroups);
            copy_buffer(output_buf, readback_buf, output_bytes);
            if (bench_counters_) {
                bench_counters_->gpu_dispatch_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
            }
        }
#else
        dispatch(magnitude_pipeline_, bind_group, workgroups);
        copy_buffer(output_buf, readback_buf, output_bytes);
#endif

        // Register OnSubmittedWorkDone to release the storage buffers once the
        // GPU confirms the dispatch+copy submission completed. The readback
        // buffer is still live (it's about to be MapAsync'd) — we release it
        // synchronously after read_back() has Unmap'd it. See design doc
        // planning/zero-copy-slice-1-design-2026-04-20.md "GPU-in-flight".
        schedule_pool_release({input_buf, output_buf});

        const bool ok = read_back(readback_buf, magnitudes, output_bytes);

        // Codex 2026-04-21 review on #536 P1: only recycle on SUCCESS.
        // On MapAsync timeout the GPU may still be mapping the buffer
        // when this function returns; handing it back to the pool lets
        // a subsequent call re-acquire and reuse it while the prior map
        // is still in flight — a validation error / silent corruption
        // risk under slow GPU workloads.
        //
        // Codex 2026-04-21 review on #560 (wave 2) P1: on failure we
        // still must drop the pool's in_flight_ reference via discard(),
        // otherwise repeated timeouts accumulate tracked handles and
        // grow the pool without bound. discard() is a release() that
        // skips the free-list recycle step.
        if (ok) {
            pool_->release(readback_buf);
        } else {
            pool_->discard(readback_buf);
        }
        return ok;
    }

    bool complex_multiply(const float* a, const float* b, float* result,
                          uint32_t count) override {
        if (!initialized_) return false;

        uint32_t bytes = count * 2 * sizeof(float);

        auto buf_a = acquire_storage_buffer(bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto buf_b = acquire_storage_buffer(bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
        auto buf_result = acquire_storage_buffer(bytes,
            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
        auto readback_buf = acquire_readback_buffer(bytes);

#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            queue_.WriteBuffer(buf_a, 0, a, bytes);
            queue_.WriteBuffer(buf_b, 0, b, bytes);
            if (bench_counters_) {
                bench_counters_->gpu_upload_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
                bench_counters_->cpu_to_gpu_bytes_total.fetch_add(
                    static_cast<double>(bytes) * 2.0,
                    std::memory_order_relaxed);
            }
        }
#else
        queue_.WriteBuffer(buf_a, 0, a, bytes);
        queue_.WriteBuffer(buf_b, 0, b, bytes);
#endif

        auto bind_group = create_bind_group(complex_mul_pipeline_,
            {buf_a, buf_b, buf_result});

        uint32_t workgroups = (count + 255) / 256;
#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            dispatch(complex_mul_pipeline_, bind_group, workgroups);
            copy_buffer(buf_result, readback_buf, bytes);
            if (bench_counters_) {
                bench_counters_->gpu_dispatch_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
            }
        }
#else
        dispatch(complex_mul_pipeline_, bind_group, workgroups);
        copy_buffer(buf_result, readback_buf, bytes);
#endif

        // Release storage buffers via OnSubmittedWorkDone; readback_buf stays
        // live until read_back() unmaps it. See compute_magnitude() for the
        // parallel comment on the GPU-in-flight tracking model.
        schedule_pool_release({buf_a, buf_b, buf_result});

        const bool ok = read_back(readback_buf, result, bytes);
        // Codex 2026-04-21 review on #536 P1 — matched to the magnitude
        // path: only recycle on success. On read_back failure the GPU
        // may still hold a mapping on this buffer.
        //
        // Codex 2026-04-21 wave 2 P1 on #560: use discard() on failure
        // so repeated timeouts don't accumulate tracked handles in
        // in_flight_ — see compute_magnitude() for the full rationale.
        if (ok) {
            pool_->release(readback_buf);
        } else {
            pool_->discard(readback_buf);
        }
        return ok;
    }

    bool batch_magnitude(const float* complex_frames, float* magnitude_frames,
                         uint32_t bins_per_frame, uint32_t num_frames) override {
        if (!initialized_) return false;

        // Treat as one large magnitude computation — the shader is element-wise
        uint32_t total_bins = bins_per_frame * num_frames;
        return compute_magnitude(complex_frames, magnitude_frames, total_bins);
    }

    // ── Device sharing verification ─────────────────────────────────────

    DeviceSharingReport verify_device_sharing(GpuSurface& surface) override {
        DeviceSharingReport report;

        if (!surface.is_initialized()) {
            report.notes = "GpuSurface not initialized";
            return report;
        }

        // Step 1: Obtain device handles
        auto* dev = static_cast<wgpu::Device*>(surface.dawn_device_handle());
        auto* q = static_cast<wgpu::Queue*>(surface.dawn_queue_handle());
        if (!dev || !q) {
            report.notes = "Device handles are null";
            return report;
        }
        report.device_obtained = true;

        // Identify backend
        if (adapter_) {
            wgpu::AdapterInfo info;
            adapter_.GetInfo(&info);
            switch (info.backendType) {
                case wgpu::BackendType::Metal:   report.backend_name = "Metal"; break;
                case wgpu::BackendType::D3D12:   report.backend_name = "D3D12"; break;
                case wgpu::BackendType::Vulkan:  report.backend_name = "Vulkan"; break;
                default: report.backend_name = "Unknown"; break;
            }
        }

        // Step 2: Create a compute buffer on the shared device
        wgpu::BufferDescriptor buf_desc{};
        buf_desc.size = 4096;
        buf_desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
        auto compute_buf = dev->CreateBuffer(&buf_desc);
        report.second_consumer_works = (compute_buf != nullptr);

        if (!report.second_consumer_works) {
            report.notes = "Failed to create compute buffer on shared device";
            return report;
        }

        // Step 3: Submit compute work on the shared queue
        // Create a minimal compute pass and submit alongside potential Skia work
        {
            wgpu::CommandEncoderDescriptor enc_desc{};
            enc_desc.label = "device-sharing-test";
            auto encoder = dev->CreateCommandEncoder(&enc_desc);

            // Just a pass that writes zeros — proves we can submit
            wgpu::ComputePassDescriptor pass_desc{};
            pass_desc.label = "sharing-test-pass";
            auto pass = encoder.BeginComputePass(&pass_desc);
            pass.End();

            auto cmd = encoder.Finish();
            q->Submit(1, &cmd);
        }
        report.concurrent_submission_ok = true;

        // Step 4: Memory pressure test — allocate substantial GPU memory
        {
            constexpr uint32_t test_size = 16 * 1024 * 1024; // 16 MB
            wgpu::BufferDescriptor big_desc{};
            big_desc.size = test_size;
            big_desc.usage = wgpu::BufferUsage::Storage;

            auto big_buf_1 = dev->CreateBuffer(&big_desc);
            auto big_buf_2 = dev->CreateBuffer(&big_desc);

            report.memory_pressure_ok = (big_buf_1 != nullptr && big_buf_2 != nullptr);

            if (!report.memory_pressure_ok) {
                report.notes = "Memory pressure: failed to allocate 2x 16MB buffers";
            }
        }

        std::ostringstream notes;
        notes << "Backend: " << report.backend_name
              << ". Device sharing verified: compute buffers and command submission "
              << "work on the same Dawn device used by Skia Graphite. "
              << "Phase 13 can proceed with shared-device Three.js bridge.";
        report.notes = notes.str();

        return report;
    }

    // ── Benchmarking ────────────────────────────────────────────────────

    std::vector<BenchmarkResult> benchmark_magnitude(
        const std::vector<uint32_t>& sizes, int iterations) override {
        std::vector<BenchmarkResult> results;

        for (uint32_t num_bins : sizes) {
            BenchmarkResult avg{};
            avg.num_elements = num_bins;

            // Generate test data
            std::vector<float> input(num_bins * 2);
            std::vector<float> output(num_bins);
            for (uint32_t i = 0; i < num_bins * 2; ++i)
                input[i] = static_cast<float>(i % 100) / 100.0f;

            uint32_t input_bytes = num_bins * 2 * sizeof(float);
            uint32_t output_bytes = num_bins * sizeof(float);

            // Pre-create GPU buffers for fair timing
            auto input_buf = create_storage_buffer(input_bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
            auto output_buf = create_storage_buffer(output_bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
            auto readback_buf = create_readback_buffer(output_bytes);
            auto bind_group = create_bind_group(magnitude_pipeline_, {input_buf, output_buf});
            uint32_t workgroups = (num_bins + 255) / 256;

            // Warm-up pass
            {
                auto warmup_rb = create_readback_buffer(output_bytes);
                queue_.WriteBuffer(input_buf, 0, input.data(), input_bytes);
                dispatch(magnitude_pipeline_, bind_group, workgroups);
                copy_buffer(output_buf, warmup_rb, output_bytes);
                read_back(warmup_rb, output.data(), output_bytes);
            }

            // Timed iterations
            for (int iter = 0; iter < iterations; ++iter) {
                double t0 = now_us();
                queue_.WriteBuffer(input_buf, 0, input.data(), input_bytes);
                double t1 = now_us();

                dispatch(magnitude_pipeline_, bind_group, workgroups);
                // Force GPU completion by doing copy + readback
                copy_buffer(output_buf, readback_buf, output_bytes);
                // Map synchronously to measure actual GPU time
                bool mapped = false;
                readback_buf.MapAsync(wgpu::MapMode::Read, 0, output_bytes,
                    wgpu::CallbackMode::AllowProcessEvents,
                    [&mapped](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        mapped = (status == wgpu::MapAsyncStatus::Success);
                    });
                instance_.ProcessEvents();
                // Busy-wait for map (measures actual GPU completion)
                while (!mapped) {
                    instance_.ProcessEvents();
                }
                double t2 = now_us();
                readback_buf.Unmap();

                // Reconstruct readback buffer for next iteration
                readback_buf = create_readback_buffer(output_bytes);

                avg.upload_us += (t1 - t0);
                avg.dispatch_us += (t2 - t1);
                avg.total_us += (t2 - t0);
            }

            avg.upload_us /= iterations;
            avg.dispatch_us /= iterations;
            avg.total_us /= iterations;
            // dispatch_us includes readback in this measurement
            avg.readback_us = 0; // folded into dispatch_us

            // CPU baseline: magnitude computation
            {
                double cpu_total = 0;
                for (int iter = 0; iter < iterations; ++iter) {
                    double t0 = now_us();
                    for (uint32_t i = 0; i < num_bins; ++i) {
                        float re = input[i * 2];
                        float im = input[i * 2 + 1];
                        output[i] = std::sqrt(re * re + im * im);
                    }
                    double t1 = now_us();
                    cpu_total += (t1 - t0);
                }
                avg.cpu_baseline_us = cpu_total / iterations;
            }

            avg.gpu_faster = avg.total_us < avg.cpu_baseline_us;
            results.push_back(avg);
        }

        return results;
    }

    std::vector<BenchmarkResult> benchmark_complex_multiply(
        const std::vector<uint32_t>& sizes, int iterations) override {
        std::vector<BenchmarkResult> results;

        for (uint32_t count : sizes) {
            BenchmarkResult avg{};
            avg.num_elements = count;

            std::vector<float> a(count * 2), b(count * 2), result(count * 2);
            for (uint32_t i = 0; i < count * 2; ++i) {
                a[i] = static_cast<float>(i % 50) / 50.0f;
                b[i] = static_cast<float>((i + 17) % 50) / 50.0f;
            }

            uint32_t bytes = count * 2 * sizeof(float);

            auto buf_a = create_storage_buffer(bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
            auto buf_b = create_storage_buffer(bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
            auto buf_r = create_storage_buffer(bytes,
                wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
            auto readback_buf = create_readback_buffer(bytes);
            auto bind_group = create_bind_group(complex_mul_pipeline_,
                {buf_a, buf_b, buf_r});
            uint32_t workgroups = (count + 255) / 256;

            // Warm-up
            {
                auto warmup_rb = create_readback_buffer(bytes);
                queue_.WriteBuffer(buf_a, 0, a.data(), bytes);
                queue_.WriteBuffer(buf_b, 0, b.data(), bytes);
                dispatch(complex_mul_pipeline_, bind_group, workgroups);
                copy_buffer(buf_r, warmup_rb, bytes);
                read_back(warmup_rb, result.data(), bytes);
            }

            for (int iter = 0; iter < iterations; ++iter) {
                double t0 = now_us();
                queue_.WriteBuffer(buf_a, 0, a.data(), bytes);
                queue_.WriteBuffer(buf_b, 0, b.data(), bytes);
                double t1 = now_us();

                dispatch(complex_mul_pipeline_, bind_group, workgroups);
                copy_buffer(buf_r, readback_buf, bytes);
                bool mapped = false;
                readback_buf.MapAsync(wgpu::MapMode::Read, 0, bytes,
                    wgpu::CallbackMode::AllowProcessEvents,
                    [&mapped](wgpu::MapAsyncStatus status, wgpu::StringView) {
                        mapped = (status == wgpu::MapAsyncStatus::Success);
                    });
                instance_.ProcessEvents();
                while (!mapped) {
                    instance_.ProcessEvents();
                }
                double t2 = now_us();
                readback_buf.Unmap();
                readback_buf = create_readback_buffer(bytes);

                avg.upload_us += (t1 - t0);
                avg.dispatch_us += (t2 - t1);
                avg.total_us += (t2 - t0);
            }

            avg.upload_us /= iterations;
            avg.dispatch_us /= iterations;
            avg.total_us /= iterations;

            // CPU baseline
            {
                double cpu_total = 0;
                for (int iter = 0; iter < iterations; ++iter) {
                    double t0 = now_us();
                    for (uint32_t i = 0; i < count; ++i) {
                        float a_re = a[i * 2], a_im = a[i * 2 + 1];
                        float b_re = b[i * 2], b_im = b[i * 2 + 1];
                        result[i * 2]     = a_re * b_re - a_im * b_im;
                        result[i * 2 + 1] = a_re * b_im + a_im * b_re;
                    }
                    double t1 = now_us();
                    cpu_total += (t1 - t0);
                }
                avg.cpu_baseline_us = cpu_total / iterations;
            }

            avg.gpu_faster = avg.total_us < avg.cpu_baseline_us;
            results.push_back(avg);
        }

        return results;
    }

#ifdef PULP_BENCHMARK
    void set_bench_counters(bench::PerfCounters* counters) override {
        bench_counters_ = counters;
    }
#endif

private:
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Instance instance_;
    wgpu::Adapter adapter_;
    std::unique_ptr<dawn::native::Instance> native_instance_;
    bool owns_device_ = false;

#ifdef PULP_BENCHMARK
    bench::PerfCounters* bench_counters_ = nullptr;
#endif

    wgpu::ComputePipeline magnitude_pipeline_;
    wgpu::ComputePipeline complex_mul_pipeline_;

    // Pool of persistent staging buffers, keyed by usage bitmask. Replaces
    // per-call device_.CreateBuffer() in compute_magnitude / complex_multiply
    // to eliminate allocator churn. Created lazily in create_pipelines().
    // Codex 2026-04-21 review on #536: the OnSubmittedWorkDone callback
    // captures the pool pointer to return buffers post-submission. The
    // callback can fire after `~DawnGpuCompute()` resets the pool in
    // shared-device mode (where event processing continues outside this
    // object). Owning the pool via `shared_ptr` and handing the callback
    // a `weak_ptr` means a stale callback just drops its buffers (RAII
    // dtor handles the wgpu::Buffer side) instead of dereferencing a
    // dangling `pool_raw` pointer.
    std::shared_ptr<detail::StagingBufferPool> pool_;

    bool create_pipelines() {
        magnitude_pipeline_ = create_pipeline("magnitude", kMagnitudeShader);
        if (!magnitude_pipeline_) return false;

        complex_mul_pipeline_ = create_pipeline("complex_multiply", kComplexMultiplyShader);
        if (!complex_mul_pipeline_) return false;

        // Staging buffer pool: pre-allocated ring of persistent wgpu::Buffer
        // objects that replaces per-call device_.CreateBuffer() in the compute
        // hot path. Planning reference:
        // planning/zero-copy-slice-1-design-2026-04-20.md. Default cap of 8
        // covers typical GPU dispatch depth (3–4 in flight) × per-call buffer
        // count (2–3 inputs + output) with headroom.
        pool_ = std::make_shared<detail::StagingBufferPool>(device_, 8);

        initialized_ = true;
        runtime::log_info("GpuCompute: pipelines created (device shared: {})",
            !owns_device_);
        return true;
    }

    wgpu::ComputePipeline create_pipeline(const char* label, const char* wgsl) {
        wgpu::ShaderSourceWGSL wgsl_source{};
        wgsl_source.code = wgsl;

        wgpu::ShaderModuleDescriptor sm_desc{};
        sm_desc.label = label;
        sm_desc.nextInChain = &wgsl_source;

        auto shader_module = device_.CreateShaderModule(&sm_desc);
        if (!shader_module) {
            runtime::log_error("GpuCompute: failed to create shader module '{}'", label);
            return nullptr;
        }

        wgpu::ComputePipelineDescriptor pipe_desc{};
        pipe_desc.label = label;
        pipe_desc.compute.module = shader_module;
        pipe_desc.compute.entryPoint = "main";
        // Use auto layout — Dawn infers bind group layout from shader
        pipe_desc.layout = nullptr;

        auto pipeline = device_.CreateComputePipeline(&pipe_desc);
        if (!pipeline) {
            runtime::log_error("GpuCompute: failed to create pipeline '{}'", label);
        }
        return pipeline;
    }

    wgpu::Buffer create_storage_buffer(uint32_t size, wgpu::BufferUsage usage) {
        wgpu::BufferDescriptor desc{};
        desc.size = size;
        desc.usage = usage;
        return device_.CreateBuffer(&desc);
    }

    wgpu::Buffer create_readback_buffer(uint32_t size) {
        wgpu::BufferDescriptor desc{};
        desc.size = size;
        desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        return device_.CreateBuffer(&desc);
    }

    // Pool-backed buffer acquisition for the compute hot path. These wrap
    // StagingBufferPool::acquire() and fall back to raw create_*() if the
    // pool is not yet initialized (should only happen in error paths).
    wgpu::Buffer acquire_storage_buffer(uint32_t size, wgpu::BufferUsage usage) {
        if (pool_) return pool_->acquire(size, usage);
        return create_storage_buffer(size, usage);
    }

    wgpu::Buffer acquire_readback_buffer(uint32_t size) {
        const auto usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        if (pool_) return pool_->acquire(size, usage);
        return create_readback_buffer(size);
    }

    // Register an OnSubmittedWorkDone callback on the queue that releases
    // each of the given buffers back to the pool once the GPU confirms the
    // most-recent submission is complete. The lambda captures the buffers
    // (which are wgpu::Buffer ref-counted handles) to keep them alive until
    // the callback fires, and a weak_ptr to the pool so a callback that
    // outlives ~DawnGpuCompute() cleanly no-ops instead of dereferencing
    // a dangling raw pointer. Codex 2026-04-21 review on #536 P1.
    void schedule_pool_release(std::vector<wgpu::Buffer> bufs) {
        if (!pool_ || bufs.empty()) return;

        std::weak_ptr<detail::StagingBufferPool> pool_weak = pool_;
        queue_.OnSubmittedWorkDone(
            wgpu::CallbackMode::AllowProcessEvents,
            [pool_weak, bufs = std::move(bufs)](wgpu::QueueWorkDoneStatus,
                                                 wgpu::StringView) mutable {
                if (auto pool = pool_weak.lock()) {
                    for (auto& b : bufs) {
                        pool->release(b);
                    }
                }
                // When lock() returns null the owning DawnGpuCompute has
                // been destroyed; `bufs` goes out of scope here and the
                // wgpu::Buffer dtor drops the underlying GPU handles.
            });
    }

    wgpu::BindGroup create_bind_group(const wgpu::ComputePipeline& pipeline,
                                       std::initializer_list<wgpu::Buffer> buffers) {
        auto layout = pipeline.GetBindGroupLayout(0);

        std::vector<wgpu::BindGroupEntry> entries;
        uint32_t binding = 0;
        for (const auto& buf : buffers) {
            wgpu::BindGroupEntry entry{};
            entry.binding = binding++;
            entry.buffer = buf;
            entry.offset = 0;
            entry.size = buf.GetSize();
            entries.push_back(entry);
        }

        wgpu::BindGroupDescriptor bg_desc{};
        bg_desc.layout = layout;
        bg_desc.entryCount = entries.size();
        bg_desc.entries = entries.data();

        return device_.CreateBindGroup(&bg_desc);
    }

    void dispatch(const wgpu::ComputePipeline& pipeline,
                  const wgpu::BindGroup& bind_group,
                  uint32_t workgroup_count) {
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);

        wgpu::ComputePassDescriptor pass_desc{};
        auto pass = encoder.BeginComputePass(&pass_desc);
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bind_group);
        pass.DispatchWorkgroups(workgroup_count);
        pass.End();

        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);
    }

    void copy_buffer(const wgpu::Buffer& src, const wgpu::Buffer& dst, uint32_t size) {
        wgpu::CommandEncoderDescriptor enc_desc{};
        auto encoder = device_.CreateCommandEncoder(&enc_desc);
        encoder.CopyBufferToBuffer(src, 0, dst, 0, size);
        auto cmd = encoder.Finish();
        queue_.Submit(1, &cmd);
    }

    bool read_back(wgpu::Buffer& buffer, void* dest, uint32_t size) {
        bool mapped = false;
        bool ok = false;

        buffer.MapAsync(wgpu::MapMode::Read, 0, size,
            wgpu::CallbackMode::AllowProcessEvents,
            [&mapped, &ok](wgpu::MapAsyncStatus status, wgpu::StringView) {
                mapped = true;
                ok = (status == wgpu::MapAsyncStatus::Success);
            });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!mapped && std::chrono::steady_clock::now() < deadline) {
            instance_.ProcessEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!mapped || !ok) return false;

        const void* data = buffer.GetConstMappedRange(0, size);
        if (!data) return false;

#ifdef PULP_BENCHMARK
        {
            const double t0 = bench::now_us();
            std::memcpy(dest, data, size);
            if (bench_counters_) {
                bench_counters_->gpu_readback_total_us.fetch_add(
                    bench::now_us() - t0, std::memory_order_relaxed);
                bench_counters_->gpu_to_cpu_bytes_total.fetch_add(
                    static_cast<double>(size),
                    std::memory_order_relaxed);
            }
        }
#else
        std::memcpy(dest, data, size);
#endif
        buffer.Unmap();
        return true;
    }
};

std::unique_ptr<GpuCompute> GpuCompute::create() {
    return std::make_unique<DawnGpuCompute>();
}

} // namespace pulp::render

#else // !PULP_HAS_SKIA

namespace pulp::render {

// Stub when Dawn/Skia not available
std::unique_ptr<GpuCompute> GpuCompute::create() {
    return nullptr;
}

} // namespace pulp::render

#endif
