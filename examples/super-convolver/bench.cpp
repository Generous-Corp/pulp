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

// ── 3. Partitioned FDL vs the current non-partitioned multi_convolve ──
// The finding-#1 payoff: does keeping the FFT at block size (partitioned FDL)
// beat one full-length-n FFT (n = next_pow2(block + IR))? One input × num_ir
// IRs → stereo, both driven through GpuCompute directly. Wall-clock (the timed
// GPU-busy variant lands with the multi_convolve_timed harness).
void bench_fdl() {
    auto gpu = pulp::render::GpuCompute::create();
    if (!gpu || !gpu->initialize_standalone()) { std::printf("\n(no GPU — skipping FDL)\n"); return; }
    std::printf("\n=== Partitioned FDL vs non-partitioned multi_convolve "
                "(block=%zu, num_ir=8) ===\n", kBlock);
    std::printf("real-time budget: %.1f us/block\n", kBudgetUs);
    std::printf("%-8s %-8s %-8s %14s %14s %10s\n",
                "IR(s)", "n_full", "n_fdl", "full us/blk", "fdl us/blk", "speedup");
    constexpr uint32_t NUM_IR = 8, BLK = static_cast<uint32_t>(kBlock), N_FDL = 2 * BLK;
    constexpr int ITERS = 60, WARM = 12;

    for (double secs : {0.25, 0.5, 1.0, 2.0}) {
        const uint32_t ir_len = static_cast<uint32_t>(secs * kSR);
        uint32_t n_full = 1; while (n_full < BLK + ir_len) n_full <<= 1;
        const uint32_t P = (ir_len + BLK - 1u) / BLK;

        std::vector<float> pl(NUM_IR, 0.5f), pr(NUM_IR, 0.5f);
        std::vector<float> in_full(2u * n_full, 0.0f), out(2u * n_full, 0.0f);
        for (uint32_t i = 0; i < BLK; ++i) in_full[2u * i] = 0.1f * std::sin(0.01f * i);

        // Non-partitioned: one full-length IR spectrum per room.
        double full_med = -1.0;
        {
            std::vector<float> ir_specs(static_cast<size_t>(2) * n_full * NUM_IR, 0.0f);
            for (size_t i = 0; i < ir_specs.size(); ++i) ir_specs[i] = 0.005f * std::sin(0.001f * i);
            if (gpu->prepare_multi_convolution(n_full, ir_specs.data(), NUM_IR)) {
                for (int w = 0; w < WARM; ++w)
                    gpu->multi_convolve(in_full.data(), pl.data(), pr.data(), out.data(), n_full, NUM_IR);
                std::vector<double> t;
                for (int r = 0; r < ITERS; ++r) { auto t0 = Clock::now();
                    gpu->multi_convolve(in_full.data(), pl.data(), pr.data(), out.data(), n_full, NUM_IR);
                    t.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count()); }
                full_med = median_us(t);
            }
        }

        // Partitioned FDL: n = 2*block, P partition spectra per room.
        double fdl_med = -1.0;
        {
            std::vector<float> part_specs(static_cast<size_t>(2) * N_FDL * NUM_IR * P, 0.0f);
            for (size_t i = 0; i < part_specs.size(); ++i) part_specs[i] = 0.005f * std::sin(0.0013f * i);
            std::vector<float> in_blk(BLK), fout(2u * BLK);
            for (uint32_t i = 0; i < BLK; ++i) in_blk[i] = 0.1f * std::sin(0.01f * i);
            if (gpu->prepare_multi_fdl(N_FDL, part_specs.data(), NUM_IR, P)) {
                for (int w = 0; w < WARM; ++w)
                    gpu->multi_fdl_convolve(in_blk.data(), pl.data(), pr.data(), fout.data(), N_FDL, NUM_IR);
                std::vector<double> t;
                for (int r = 0; r < ITERS; ++r) { auto t0 = Clock::now();
                    gpu->multi_fdl_convolve(in_blk.data(), pl.data(), pr.data(), fout.data(), N_FDL, NUM_IR);
                    t.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count()); }
                fdl_med = median_us(t);
            }
        }

        if (full_med > 0 && fdl_med > 0)
            std::printf("%-8.2f %-8u %-8u %14.1f %14.1f %9.2fx\n",
                        secs, n_full, N_FDL, full_med, fdl_med, full_med / fdl_med);
        else
            std::printf("%-8.2f %-8u %-8u %14s %14s %10s\n", secs, n_full, N_FDL,
                        full_med > 0 ? "" : "(fail)", fdl_med > 0 ? "" : "(fail)", "n/a");
    }
    std::printf("\nspeedup > 1 = the partitioned FDL beats the non-partitioned kernel.\n");
    std::printf("This is the number that settles the SuperConvolver v1.2 KILL verdict.\n");
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
    bench_fdl();
    return 0;
}
