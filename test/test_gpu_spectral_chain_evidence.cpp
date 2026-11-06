#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/bench/perf_counters.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

namespace {
using pulp::gpu_audio::CpuSpectralStack;
using pulp::gpu_audio::GpuSpectralStack;
using pulp::render::GpuCompute;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFft = 2048;
constexpr uint32_t kHop = 512;
constexpr uint32_t kLayers = 16;
constexpr int kWarmup = 8;
constexpr int kSamples = 64;

struct Summary {
    std::vector<double> us;
    double min = 0, p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0, jitter = 0;
};

Summary summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    Summary s;
    s.us = values;
    if (values.empty()) return s;
    const auto q = [&](double p) {
        const auto i = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
        return values[i];
    };
    s.min = values.front(); s.p50 = q(.50); s.p95 = q(.95); s.p99 = q(.99);
    s.max = values.back();
    s.mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double ss = 0.0;
    for (double v : values) ss += (v - s.mean) * (v - s.mean);
    s.jitter = std::sqrt(ss / values.size());
    return s;
}

void fill_frame(std::vector<float>& frame, uint32_t bin) {
    for (uint32_t i = 0; i < frame.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(frame.size());
        frame[i] = 0.4f * std::sin(2.0f * 3.1415926535f * bin * t)
                 + 0.2f * std::sin(2.0f * 3.1415926535f * (bin + 7) * t);
    }
}

template <typename Stack>
Summary run(Stack& stack, const std::vector<float>& frame) {
    std::vector<float> out(kFft, 0.0f);
    const float weights[kLayers] = {
        1.0f, .9f, .8f, .7f, .6f, .5f, .4f, .3f,
        .2f, .3f, .4f, .5f, .6f, .7f, .8f, .9f};
    for (uint32_t i = 0; i < kLayers; ++i)
        if (!stack.capture(i, frame.data())) return {};
    for (int i = 0; i < kWarmup; ++i)
        if (!stack.render(out.data(), weights, 0.35f, 0.0f)) return {};
    std::vector<double> samples;
    samples.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const auto t0 = Clock::now();
        if (!stack.render(out.data(), weights, 0.35f, 0.0f)) return {};
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return summarize(std::move(samples));
}

void print_summary(const char* name, const Summary& s) {
    std::cout << name << " min_us=" << s.min << " p50_us=" << s.p50
              << " p95_us=" << s.p95 << " p99_us=" << s.p99
              << " max_us=" << s.max << " mean_us=" << s.mean
              << " jitter_sd_us=" << s.jitter << '\n';
}
} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    std::vector<float> frame(kFft);
    fill_frame(frame, 37);

    CpuSpectralStack cpu;
    if (!cpu.prepare(kFft, kHop, kLayers)) {
        std::cerr << "FAIL cpu_prepare\n";
        return 2;
    }
    const auto cpu_summary = run(cpu, frame);
    print_summary("cpu", cpu_summary);
    const size_t cpu_payload = static_cast<size_t>(kLayers) * kFft * 2 * sizeof(float);
    std::cout << "cpu_resident_payload_bytes=" << cpu_payload
              << " gpu_copy_count_measured=false\n";

    auto gpu = GpuCompute::create();
    if (!gpu || !gpu->initialize_standalone()) {
        std::cout << "gpu_available=false reason=initialize_standalone\n";
        return 0;
    }
    const auto caps = gpu->capabilities();
    std::cout << "gpu_available=true backend=" << caps.backend
              << " vendor=" << caps.vendor
              << " timestamp_query=" << (caps.timestamp_query ? "true" : "false")
              << " max_fft=" << caps.max_fft_size << '\n';

    GpuSpectralStack stack;
    if (!stack.prepare(kFft, kHop, kLayers, gpu.get()) || !stack.available()) {
        std::cout << "gpu_available=false reason=prepare_spectral_stack\n";
        return 0;
    }
    const auto gpu_summary = run(stack, frame);
    print_summary("gpu", gpu_summary);
    const size_t gpu_payload = static_cast<size_t>(kLayers) * kFft * 2 * sizeof(float);
    std::cout << "gpu_resident_payload_bytes=" << gpu_payload
              << " resident_output_bytes_per_hop=" << (2 * kFft * sizeof(float))
              << " gpu_copy_count_measured=false\n";

    // Positive oracle: the resident stack must produce non-silent output and
    // stay within the existing CPU/GPU tolerance used by the spectral tests.
    CpuSpectralStack cpu_oracle;
    cpu_oracle.prepare(kFft, kHop, kLayers);
    std::vector<float> cpu_out(kFft), gpu_out(kFft);
    const float weights[kLayers] = {
        1.0f, .9f, .8f, .7f, .6f, .5f, .4f, .3f,
        .2f, .3f, .4f, .5f, .6f, .7f, .8f, .9f};
    for (uint32_t i = 0; i < kLayers; ++i) {
        cpu_oracle.capture(i, frame.data());
        stack.clear(i);
        stack.capture(i, frame.data());
    }
    cpu_oracle.render(cpu_out.data(), weights, .35f, 0.0f);
    stack.render(gpu_out.data(), weights, .35f, 0.0f);
    double max_diff = 0.0;
    for (uint32_t i = 0; i < kFft; ++i)
        max_diff = std::max(max_diff, static_cast<double>(std::abs(cpu_out[i] - gpu_out[i])));
    std::cout << "positive_parity_max_abs=" << max_diff
              << " positive_non_silent=" << (max_diff >= 0.0 && std::abs(gpu_out[37]) > 1e-7f ? "true" : "false") << '\n';

    // Planted negative 1: a resource shape beyond the device limit must be
    // refused as unavailable rather than treated as a working GPU chain.
    GpuSpectralStack oversized;
    oversized.prepare(1u << 20, 1u << 19, 1u << 20, gpu.get());
    std::cout << "negative_oversized_available=" << (oversized.available() ? "true" : "false") << '\n';

    // Planted negative 2: device-loss notification must close GPU admission;
    // the existing CPU stack remains the explicit fallback path.
    gpu->notify_device_lost();
    std::vector<float> lost_out(kFft, 0.0f), fallback_out(kFft, 0.0f);
    const bool gpu_after_loss = stack.render(lost_out.data(), weights, .35f, 0.0f);
    const bool cpu_fallback = cpu_oracle.render(fallback_out.data(), weights, .35f, 0.0f);
    std::cout << "negative_device_loss device_lost=" << (gpu->device_lost() ? "true" : "false")
              << " gpu_render_after_loss=" << (gpu_after_loss ? "true" : "false")
              << " cpu_fallback_render=" << (cpu_fallback ? "true" : "false") << '\n';
    return (max_diff < 0.02 && std::abs(gpu_out[37]) > 1e-7f &&
            !oversized.available() && !gpu_after_loss && cpu_fallback) ? 0 : 3;
}
