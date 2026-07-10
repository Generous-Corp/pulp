// SuperConvolver convolution benchmark — honest CPU vs GPU per-block timing.
//
// Establishes ground truth for two regimes:
//
//   1. Single stereo convolution: signal::PartitionedConvolver (the live CPU
//      path) vs gpu_audio::GpuConvolver (full-IR-size FFT every block). The CPU
//      partitioned path is expected to WIN here — the per-block full-IR FFT plus
//      two separate channel readbacks make the naive GPU convolver
//      algorithmically worse than CPU partitioning. This documents that reality.
//
//   2. Massive multi-IR convolution: ONE input convolved against N distinct IRs
//      ("rooms"/"taps") in a single batched GPU submit
//      (gpu_audio::GpuMultiConvolver) vs N independent CPU PartitionedConvolvers.
//      This is the regime where the GPU structurally wins: on the GPU it is one
//      batch (one submit, one readback); on the CPU it is N× the partitioned
//      work. We report the N at which the CPU exceeds the real-time budget while
//      the GPU does not.
//
// macOS + GPU only (gated in CMake like the screenshot target). All timings are
// wall-clock medians over many iterations after warmup. No fabricated numbers:
// if the GPU device is absent the bench prints that and exits 0.

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>
#include <pulp/signal/convolver.hpp>

#include "super_convolver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using pulp::examples::make_reverb_ir;

double median_us(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::vector<float> noise_block(std::size_t n, std::uint32_t seed) {
    std::vector<float> b(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    for (auto& s : b) s = d(rng);
    return b;
}

constexpr double kSR = 48000.0;
constexpr std::size_t kBlock = 512;
constexpr double kBudgetUs = static_cast<double>(kBlock) / kSR * 1e6;  // 10666.7 us

// ── 1. Single stereo convolution: CPU partitioned vs naive GPU full-IR FFT ──
void bench_single() {
    std::printf("\n=== Single stereo convolution (block=%zu, %.0f kHz) ===\n",
                kBlock, kSR / 1000.0);
    std::printf("real-time budget: %.1f us/block\n", kBudgetUs);
    std::printf("%-10s %14s %14s %10s\n", "IR (s)", "CPU us/blk", "GPU us/blk", "winner");

    const double ir_secs[] = {0.1, 0.5, 1.0, 2.0, 4.0};
    constexpr int kIters = 200, kWarm = 20;

    for (double secs : ir_secs) {
        const std::size_t ir_len = static_cast<std::size_t>(secs * kSR);
        const auto ir = make_reverb_ir(ir_len);

        // CPU: stereo = 2 partitioned convolvers.
        pulp::signal::PartitionedConvolver cpu_l, cpu_r;
        cpu_l.load_ir(ir.data(), ir.size(), kBlock);
        cpu_r.load_ir(ir.data(), ir.size(), kBlock);
        auto xl = noise_block(kBlock, 1), xr = noise_block(kBlock, 2);
        std::vector<float> yl(kBlock), yr(kBlock);

        std::vector<double> cpu;
        for (int it = 0; it < kIters + kWarm; ++it) {
            auto t0 = Clock::now();
            cpu_l.process(xl.data(), yl.data(), kBlock);
            cpu_r.process(xr.data(), yr.data(), kBlock);
            auto t1 = Clock::now();
            if (it >= kWarm)
                cpu.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        const double cpu_med = median_us(cpu);

        // GPU: gpu_audio::GpuConvolver processes both channels per call.
        pulp::gpu_audio::GpuConvolver gpu(2, static_cast<uint32_t>(kBlock),
                                          static_cast<uint32_t>(kSR), ir);
        double gpu_med = -1.0;
        if (gpu.prepare() && gpu.gpu_available()) {
            const float* in_ptrs[2] = {xl.data(), xr.data()};
            float* out_ptrs[2] = {yl.data(), yr.data()};
            pulp::audio::BufferView<const float> iv(in_ptrs, 2, kBlock);
            pulp::audio::BufferView<float> ov(out_ptrs, 2, kBlock);
            std::vector<double> gpu_t;
            for (int it = 0; it < kIters + kWarm; ++it) {
                auto t0 = Clock::now();
                gpu.process_block(iv, ov, static_cast<uint32_t>(kBlock));
                auto t1 = Clock::now();
                if (it >= kWarm)
                    gpu_t.push_back(
                        std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            gpu_med = median_us(gpu_t);
        }

        const char* win = gpu_med < 0 ? "n/a"
                        : (gpu_med < cpu_med ? "GPU" : "CPU");
        if (gpu_med < 0)
            std::printf("%-10.2f %14.1f %14s %10s\n", secs, cpu_med, "(no gpu)", win);
        else
            std::printf("%-10.2f %14.1f %14.1f %10s\n", secs, cpu_med, gpu_med, win);
    }
}

// Constant-power pan weights matching GpuMultiConvolver::prepare() so the CPU
// baseline produces the same irreducible stereo output (N rooms each panned to a
// distinct position, scaled by 1/sqrt(N)).
void make_pans(uint32_t N, std::vector<float>& pl, std::vector<float>& pr) {
    pl.assign(N, 0.0f);
    pr.assign(N, 0.0f);
    const float norm = 1.0f / std::sqrt(static_cast<float>(N));
    for (uint32_t k = 0; k < N; ++k) {
        const float t = (N == 1) ? 0.5f : static_cast<float>(k) / static_cast<float>(N - 1);
        const float theta = t * 1.57079632679f;
        pl[k] = std::cos(theta) * norm;
        pr[k] = std::sin(theta) * norm;
    }
}

// ── 2. Multi-IR convolution: N panned rooms — one GPU batch vs N CPU convs ──
void bench_multi() {
    std::printf("\n=== Multi-IR convolution: N panned rooms -> stereo "
                "(block=%zu, %.0f kHz) ===\n", kBlock, kSR / 1000.0);
    std::printf("real-time budget: %.1f us/block. Each room is panned to a "
                "distinct stereo position\n(irreducible: not collapsible to one "
                "summed IR).\n", kBudgetUs);
    std::printf("%-8s %-8s %14s %14s %10s %12s %12s\n",
                "IR (s)", "N", "CPU us/blk", "GPU us/blk", "speedup",
                "CPU>budget", "GPU>budget");

    const double ir_secs[] = {0.25, 0.5, 1.0};
    // Pushed up to the GPU buffer cap (num_ir * fft_size <= 1<<22): 256 @0.25s,
    // 128 @0.5s, 64 @1s. Over the cap the GPU plan can't build (prints no-gpu),
    // which is itself an honest limit data point.
    const uint32_t counts[] = {4, 8, 16, 32, 64, 128, 256};
    constexpr int kIters = 100, kWarm = 10;

    for (double secs : ir_secs) {
        const std::size_t ir_len = static_cast<std::size_t>(secs * kSR);

        for (uint32_t N : counts) {
            // N distinct IRs (different seeds == different rooms).
            std::vector<std::vector<float>> irs(N);
            for (uint32_t k = 0; k < N; ++k)
                irs[k] = make_reverb_ir(ir_len, 0x1000u + k * 2654435761u);

            std::vector<float> pl, pr;
            make_pans(N, pl, pr);

            // CPU: N independent partitioned convolvers, each panned into L/R.
            std::vector<pulp::signal::PartitionedConvolver> cpu(N);
            for (uint32_t k = 0; k < N; ++k)
                cpu[k].load_ir(irs[k].data(), irs[k].size(), kBlock);
            auto x = noise_block(kBlock, 7);
            std::vector<float> y(kBlock), accL(kBlock), accR(kBlock);

            std::vector<double> cpu_t;
            for (int it = 0; it < kIters + kWarm; ++it) {
                auto t0 = Clock::now();
                std::fill(accL.begin(), accL.end(), 0.0f);
                std::fill(accR.begin(), accR.end(), 0.0f);
                for (uint32_t k = 0; k < N; ++k) {
                    cpu[k].process(x.data(), y.data(), kBlock);
                    for (std::size_t i = 0; i < kBlock; ++i) {
                        accL[i] += pl[k] * y[i];
                        accR[i] += pr[k] * y[i];
                    }
                }
                auto t1 = Clock::now();
                if (it >= kWarm)
                    cpu_t.push_back(
                        std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            const double cpu_med = median_us(cpu_t);

            // GPU: one batched submit over N IRs + GPU pan-reduce to stereo.
            pulp::gpu_audio::GpuMultiConvolver gpu(
                static_cast<uint32_t>(kBlock), static_cast<uint32_t>(kSR), irs);
            double gpu_med = -1.0;
            if (gpu.prepare() && gpu.gpu_available()) {
                std::vector<float> wl(kBlock), wr(kBlock);
                std::vector<double> gpu_t;
                bool all_ok = true;
                for (int it = 0; it < kIters + kWarm; ++it) {
                    auto t0 = Clock::now();
                    const bool ok = gpu.convolve_stereo(
                        x.data(), wl.data(), wr.data(), static_cast<uint32_t>(kBlock));
                    auto t1 = Clock::now();
                    all_ok = all_ok && ok;
                    if (it >= kWarm)
                        gpu_t.push_back(
                            std::chrono::duration<double, std::micro>(t1 - t0).count());
                }
                // Guard against a silent error-buffer no-op: noise in + nonzero
                // IRs MUST yield nonzero, finite output. Reject otherwise.
                double energy = 0.0;
                for (std::size_t i = 0; i < kBlock; ++i)
                    energy += wl[i] * wl[i] + wr[i] * wr[i];
                const bool real_output = std::isfinite(energy) && energy > 1e-12;
                // Only report a GPU number if every block genuinely produced GPU
                // output — never time a silently-failed no-op.
                if (all_ok && real_output) gpu_med = median_us(gpu_t);
            }

            if (gpu_med < 0) {
                std::printf("%-8.2f %-8u %14.1f %14s %10s %12s %12s\n",
                            secs, N, cpu_med, "(no gpu)", "-", "-", "-");
            } else {
                std::printf("%-8.2f %-8u %14.1f %14.1f %9.2fx %12s %12s\n",
                            secs, N, cpu_med, gpu_med, cpu_med / gpu_med,
                            cpu_med > kBudgetUs ? "YES" : "no",
                            gpu_med > kBudgetUs ? "YES" : "no");
            }
        }
        std::printf("\n");
    }
}

}  // namespace

// Roofline probe for the multi-IR path (roofline audit finding #1). Drives
// GpuCompute::multi_convolve_timed directly (like gpu_hammer) so `n` (FFT size)
// is controlled independently, and reports GPU-busy time alongside wall time.
//
// Two questions:
//   1. Is the kernel GPU-busy (compute/bandwidth-bound) or overhead-bound?
//      -> compare gpu_busy_us against wall_us. If gpu_busy ≈ wall, the cost is
//         real GPU work, not dispatch/readback overhead.
//   2. Does the cost scale with the FULL FFT size n rather than useful IR taps?
//      -> the non-partitioned kernel uses n = next_pow2(block + IR_len), so cost
//         should track n·log2(n)·num_ir. A partitioned FDL would instead track
//         (IR_len/block) small block-size FFTs — the removable waste.
void bench_multi_roofline() {
    auto gpu = pulp::render::GpuCompute::create();
    const bool have_gpu = gpu && gpu->initialize_standalone();
    std::printf("\n=== multi-IR roofline: GPU-busy vs wall, cost vs FFT size ===\n");
    if (!have_gpu || !gpu->capabilities().timestamp_query) {
        std::printf("(timestamp queries unavailable — skipping GPU-busy measurement)\n");
        return;
    }
    constexpr uint32_t kBlock = 512, kNumIr = 32;
    std::printf("block=%u num_ir=%u | a partitioned FDL would cost ~(IR_len/block)"
                " block-size FFTs instead of one length-n FFT.\n\n", kBlock, kNumIr);
    std::printf("%-8s %-8s %12s %12s %9s %16s\n",
                "IR(s)", "n", "wall_us", "gpu_busy", "busy/wall", "us/(n·log·Nir)");

    for (double ir_sec : {0.1, 0.25, 0.5, 1.0, 2.0}) {
        const uint32_t ir_len = static_cast<uint32_t>(ir_sec * 48000.0);
        uint32_t n = 1; while (n < kBlock + ir_len) n <<= 1;

        // Content-agnostic IR spectra (timing depends on n and num_ir, not values).
        std::vector<float> ir_specs(static_cast<size_t>(2) * n * kNumIr);
        for (size_t i = 0; i < ir_specs.size(); ++i)
            ir_specs[i] = 0.01f * std::sin(0.001f * static_cast<float>(i));
        if (!gpu->prepare_multi_convolution(n, ir_specs.data(), kNumIr)) {
            std::printf("%-8.2f %-8u  prepare failed (over storage-buffer limit)\n", ir_sec, n);
            continue;
        }
        std::vector<float> in(2u * n, 0.0f), out(2u * n, 0.0f);
        for (uint32_t i = 0; i < n; ++i) in[2u * i] = 0.1f * std::sin(0.01f * i);
        std::vector<float> pl(kNumIr, 0.7f), pr(kNumIr, 0.7f);

        double busy = -1.0;
        for (int w = 0; w < 8; ++w)
            gpu->multi_convolve_timed(in.data(), pl.data(), pr.data(), out.data(), n, kNumIr, &busy);
        std::vector<double> wall, gbusy;
        for (int r = 0; r < 40; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            gpu->multi_convolve_timed(in.data(), pl.data(), pr.data(), out.data(), n, kNumIr, &busy);
            wall.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - t0).count());
            if (busy >= 0.0) gbusy.push_back(busy);
        }
        const double w_med = median_us(wall);
        const double b_med = gbusy.empty() ? -1.0 : median_us(gbusy);
        const double nlogn_nir = double(n) * std::log2(double(n)) * kNumIr;
        std::printf("%-8.2f %-8u %12.1f %12.1f %8.0f%% %16.4f\n",
                    ir_sec, n, w_med, b_med,
                    b_med > 0 ? 100.0 * b_med / w_med : 0.0,
                    b_med > 0 ? b_med / nlogn_nir * 1e3 : 0.0);
    }
    std::printf("\nReading: busy/wall RISING toward ~1.0 as n grows means the cost is\n");
    std::printf("real GPU work, not dispatch/readback overhead — so a fix must cut the\n");
    std::printf("WORK, not the overhead. gpu_busy grows with n (the full FFT size, set\n");
    std::printf("by IR length), which is the removable waste: a partitioned FDL keeps\n");
    std::printf("n at the block size and does IR_len/block small-FFT MAC rows instead.\n");
    std::printf("(us/(n·log·Nir) falling, not flat, shows the kernel gains occupancy at\n");
    std::printf("large n — the same under-parallelization the audit found at small n.)\n");
    std::printf("See planning/2026-07-09-roofline-audit-audio-pipeline.md #1.\n");
}

int main() {
    std::printf("SuperConvolver convolution benchmark (Apple GPU / Metal)\n");
    if (auto probe = pulp::render::GpuCompute::create();
        probe && probe->initialize_standalone()) {
        const auto c = probe->capabilities();
        std::printf("device: %s %s | max storage-buffer binding: %.0f MiB\n",
                    c.vendor.c_str(), c.backend.c_str(),
                    c.max_storage_buffer_binding_size / (1024.0 * 1024.0));
    } else {
        std::printf("no GPU compute device available — printing CPU columns only\n");
    }
    bench_single();
    bench_multi();
    bench_multi_roofline();
    return 0;
}
