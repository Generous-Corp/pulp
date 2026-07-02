// Headless performance harness for the GPU NAM processor. Measures the
// audio-thread cost of process() for the CPU oracle and the GPU engine across a
// range of host block sizes, and reports the realtime ratio (how many times
// faster than realtime a single instance runs) plus the GPU worker's per-forward
// cost. No GPU window, no audio device.
//
//   gpu-nam-bench [model.nam ...]
//
// With no arguments it benches the bundled default model. Pass one or more .nam
// paths to bench specific captures (any supported architecture). Numbers are an
// internal tuning gauge for this machine — they are not a portable benchmark.

#include "gpu_nam_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using pulp::examples::GpuNamProcessor;
using clock_type = std::chrono::steady_clock;

// Amortized mean per-block time. Mean (not median) is the honest statistic here:
// the 512-sample re-block FIFO makes sub-512 host blocks bimodal (a cheap FIFO
// append most blocks, a full model run every Nth), and only the mean captures the
// model's true per-sample cost spread across those blocks.
double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

// Deterministic guitar-ish drive: a couple of partials plus a little noise.
std::vector<float> make_signal(std::size_t n) {
    std::vector<float> s(n);
    std::uint32_t r = 0x1234567u;
    for (std::size_t i = 0; i < n; ++i) {
        r = r * 1664525u + 1013904223u;
        const float noise = static_cast<float>(r >> 9) / 4194304.0f - 1.0f;
        s[i] = 0.4f * std::sin(0.05f * i) + 0.2f * std::sin(0.11f * i) + 0.02f * noise;
    }
    return s;
}

struct Row {
    std::size_t block;
    double cpu_us;      // median process() wall time, CPU engine
    double cpu_rt;      // realtime ratio, CPU engine
    double gpu_us;      // median process() wall time, GPU engine (audio thread)
    double gpu_worker_us;
    double gpu_rt_percent;
    bool gpu_active;
    std::string backend;
};

// Run `nblocks` host blocks of `block` samples through a freshly prepared
// processor on the given engine and return the median per-block process() time
// (µs). When `pace` is set, sleep one block of wall-clock between calls so the
// non-realtime GPU worker gets the gap it has in a live host.
double bench_engine(const std::string& model_path, double sr, std::size_t block,
                    int nblocks, float engine, bool pace, GpuNamProcessor::GpuStatus* status) {
    pulp::state::StateStore store;
    GpuNamProcessor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(pulp::examples::kInputGain, 3.0f);
    store.set_value(pulp::examples::kMix, 100.0f);
    store.set_value(pulp::examples::kBypass, 0.0f);
    store.set_value(pulp::examples::kEngine, engine);
    if (!model_path.empty()) proc.load_model(model_path);

    pulp::format::PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(block);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);

    const auto sig = make_signal(block);
    std::vector<float> l(block), r(block), ol(block), orr(block);
    for (std::size_t i = 0; i < block; ++i) l[i] = r[i] = sig[i];
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = sr;
    pctx.num_samples = static_cast<int>(block);
    const double block_seconds = static_cast<double>(block) / sr;

    std::vector<double> times;
    times.reserve(nblocks);
    for (int b = 0; b < nblocks; ++b) {
        const float* inp[2] = {l.data(), r.data()};
        float* outp[2] = {ol.data(), orr.data()};
        pulp::audio::BufferView<const float> in(inp, 2, block);
        pulp::audio::BufferView<float> ob(outp, 2, block);
        const auto t0 = clock_type::now();
        proc.process(ob, in, mi, mo, pctx);
        const auto t1 = clock_type::now();
        // Discard the first few blocks (cold caches, engine warm-up).
        if (b >= 8)
            times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        if (pace) std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long>(block_seconds * 1e6)));
    }
    if (status) *status = proc.gpu_status();
    proc.release();
    return mean(times);
}

Row bench_block(const std::string& model_path, double sr, std::size_t block) {
    Row row{};
    row.block = block;
    // Enough blocks to fill the re-block FIFO and average out scheduling jitter.
    const int nblocks = 96;
    row.cpu_us = bench_engine(model_path, sr, block, nblocks, 0.0f, false, nullptr);
    row.cpu_rt = row.cpu_us > 0.0 ? (static_cast<double>(block) / sr * 1e6) / row.cpu_us : 0.0;

    GpuNamProcessor::GpuStatus gs;
    row.gpu_us = bench_engine(model_path, sr, block, nblocks, 1.0f, true, &gs);
    row.gpu_active = gs.active;
    row.backend = gs.backend;
    row.gpu_worker_us = gs.avg_us;
    row.gpu_rt_percent = gs.rt_percent;
    return row;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> models;
    for (int i = 1; i < argc; ++i) models.emplace_back(argv[i]);
    if (models.empty()) models.emplace_back("");  // bundled default

    constexpr double SR = 48000.0;
    const std::size_t blocks[] = {64, 128, 256, 512, 1024};

    for (const auto& m : models) {
        // Resolve a display name + architecture by loading once.
        pulp::examples::nam::NamRuntime probe;
        std::string err;
        const std::string path = m.empty() ? std::string(GPU_NAM_DEFAULT_MODEL_PATH) : m;
        const bool ok = pulp::examples::nam::load_nam_runtime(path, probe, &err);
        std::printf("\n== %s (%s) @ %.0f Hz ==\n",
                    path.c_str(), ok ? probe.arch_name() : "load failed", SR);
        if (!ok) { std::printf("   %s\n", err.c_str()); continue; }
        std::printf("   %-6s | %-12s %-8s | %-12s %-10s %-8s %s\n",
                    "block", "CPU us/blk", "CPU xRT", "GPU us/blk", "GPU wrk us",
                    "GPU %RT", "backend");
        for (std::size_t b : blocks) {
            const Row row = bench_block(m, SR, b);
            std::printf("   %-6zu | %-12.1f %-8.1f | %-12.1f %-10.1f %-8.1f %s\n",
                        row.block, row.cpu_us, row.cpu_rt, row.gpu_us,
                        row.gpu_worker_us, row.gpu_rt_percent,
                        row.gpu_active ? row.backend.c_str() : "(cpu fallback)");
        }
    }
    std::printf("\nxRT = realtime multiple for one instance (higher is faster). "
                "Internal gauge for this machine only.\n");
    return 0;
}
