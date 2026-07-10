// GPU roofline / occupancy harness for Pulp's gpu_audio compute passes.
//
// For each GPU compute pass it drives the real pulp::render::GpuCompute API over
// escalating workloads and reports, per pass:
//   * achieved GMAC/s (measured multiply-adds / measured GPU-block wall time)
//   * device roofline GMAC/s (measured empirically from a large, well-parallel
//     matmul — the practical fused-MAC peak this device sustains)
//   * roofline gap  = peak / achieved   (how far below the device we run)
//   * lane occupancy = lanes doing useful work / lanes launched (analytic, from
//     the pass's workgroup_size and the (dispatch × workgroup_size) launch)
//   * workgroups dispatched (SM/core-coverage proxy — a single workgroup pins
//     the whole grid to one core)
//   * per-thread serial MAC depth (the reduction each lane walks alone)
//
// It then prints a table ranked by (roofline gap × hotness), where hotness is the
// measured block time — the biggest, hottest gaps float to the top. This is the
// instrument that confirms the WaveNet one-thread-per-sample occupancy gap and
// surfaces its siblings (matmul / additive / modal are all one-thread-per-output).
//
// Tooling only; built under PULP_ENABLE_GPU. Needs a real GPU/Dawn device.

#include "pulp/render/gpu_compute.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using pulp::render::GpuCompute;
using Clock = std::chrono::steady_clock;

namespace {

// Median wall time (ms) of `fn` over `iters` timed runs after `warmup` runs.
template <typename Fn>
double median_ms(Fn&& fn, int iters, int warmup = 3) {
    for (int i = 0; i < warmup; ++i) fn();
    std::vector<double> ts;
    ts.reserve(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        ts.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ts.begin(), ts.end());
    return ts[ts.size() / 2];
}

struct Row {
    std::string pass;
    std::string shape;
    double macs = 0;          // multiply-adds per call
    double ms = 0;            // median block time (ms)
    double gmacs = 0;         // achieved GMAC/s
    double occupancy = 0;     // lanes busy / lanes launched (0..1)
    uint32_t workgroups = 0;  // workgroups dispatched
    double serial_depth = 0;  // MACs one lane walks serially
    double gap = 0;           // peak_gmacs / achieved (filled after peak known)
};

// Analytic launch geometry: lanes-busy / (workgroups * workgroup_size).
double occupancy_of(uint64_t lanes_busy, uint32_t workgroups, uint32_t wg_size) {
    const double launched = static_cast<double>(workgroups) * wg_size;
    return launched > 0 ? static_cast<double>(lanes_busy) / launched : 0.0;
}

// WaveNet weight-blob length for one array, matching prepare_wavenet's layout
// (see test/test_gpu_compute.cpp determinism case): rechannel C + per-layer
// (convW ZxCxK + convB Z + mixin Z + 1x1 CxC + 1x1 b C) + head rechannel HxC.
uint32_t wavenet_weights_len(uint32_t C, uint32_t K, uint32_t L, uint32_t H,
                             uint32_t Z) {
    return C + L * (Z * C * K + Z + Z + C * C + C) + H * C;
}

}  // namespace

int main() {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) {
        std::printf("gpu-roofline: no GPU/Dawn device available — skipping.\n");
        return 0;  // treat as skip, like the GPU tests
    }

    const auto caps = compute->capabilities();
    std::printf("=== GPU roofline / occupancy harness ===\n");
    std::printf("backend=%s vendor=%s timestamp_query=%d "
                "max_wg_invocations=%u max_wg_x=%u\n\n",
                caps.backend.c_str(), caps.vendor.c_str(), caps.timestamp_query,
                caps.max_compute_invocations_per_workgroup,
                caps.max_compute_workgroup_size_x);

    std::vector<Row> rows;

    // ── Device roofline probe ────────────────────────────────────────────────
    // A large square matmul is embarrassingly parallel (one thread per output
    // element, K-deep reduction each). Its best achieved GMAC/s is the practical
    // fused-MAC peak we hold every other pass against.
    double peak_gmacs = 0;
    {
        for (uint32_t N : {256u, 384u, 512u}) {
            const uint32_t M = N, K = N;
            std::vector<float> a(static_cast<std::size_t>(M) * K, 0.5f);
            std::vector<float> b(static_cast<std::size_t>(K) * N, 0.25f);
            std::vector<float> c(static_cast<std::size_t>(M) * N, 0.0f);
            const double macs = static_cast<double>(M) * N * K;
            const double ms = median_ms(
                [&] { compute->matmul(a.data(), b.data(), c.data(), M, K, N); }, 20);
            const double gmacs = macs / (ms * 1e6);
            peak_gmacs = std::max(peak_gmacs, gmacs);
            // matmul shader: workgroup_size 64, one thread per output element.
            const uint32_t wg = (M * N + 63u) / 64u;
            rows.push_back({"matmul", std::to_string(N) + "^3", macs, ms, gmacs,
                            occupancy_of(static_cast<uint64_t>(M) * N, wg, 64), wg,
                            static_cast<double>(K), 0});
        }
    }
    std::printf("device roofline (best matmul GMAC/s): %.1f GMAC/s\n\n", peak_gmacs);

    // ── WaveNet layer pass — the target ──────────────────────────────────────
    // Real .nam topologies. one-thread-per-sample: dispatch = ceil(B/256)
    // workgroups of 256 lanes, only B active; each lane walks the whole
    // per-sample reduction serially.
    struct WnCfg { const char* name; uint32_t C, K, L, H, gated; };
    const WnCfg wn_cfgs[] = {
        {"nam-lite  C8 K3 L2",   8, 3, 2, 1, 1},
        {"nam-feather C12 K3 L4", 12, 3, 4, 1, 1},
        {"nam-standard C16 K3 L10", 16, 3, 10, 1, 1},
    };
    for (const auto& cfg : wn_cfgs) {
        for (uint32_t B : {64u, 128u, 256u, 512u}) {
            const uint32_t Z = (cfg.gated ? 2u : 1u) * cfg.C;
            std::vector<uint32_t> dils(cfg.L);
            for (uint32_t i = 0; i < cfg.L; ++i) dils[i] = 1u << i;  // 1,2,4,...
            GpuCompute::WavenetLayerArraySpec spec;
            spec.input_size = 1; spec.condition_size = 1; spec.channels = cfg.C;
            spec.kernel = cfg.K; spec.head_size = cfg.H; spec.gated = cfg.gated;
            spec.head_bias = 0; spec.dilations = dils.data();
            spec.num_layers = cfg.L;

            const uint32_t need = wavenet_weights_len(cfg.C, cfg.K, cfg.L, cfg.H, Z);
            std::vector<float> w(need + 1);  // +1 trailing head_scale
            for (uint32_t i = 0; i < w.size(); ++i)
                w[i] = 0.05f * std::sin(0.017f * i) - 0.01f;
            if (!compute->prepare_wavenet(&spec, 1, w.data(),
                                          static_cast<uint32_t>(w.size()), B, 0.8f)) {
                std::printf("  (prepare_wavenet failed for %s B=%u)\n", cfg.name, B);
                continue;
            }
            std::vector<float> in(B), out(B, 0.0f);
            for (uint32_t i = 0; i < B; ++i) in[i] = 0.2f * std::sin(0.03f * i);

            const double ms = median_ms(
                [&] { compute->wavenet_forward(in.data(), out.data(), B); }, 50);

            // MACs per sample: per layer conv Z*K*C + mixin Z + 1x1 C*C, plus
            // rechannel C*1 and head H*C, times B samples.
            const double per_sample =
                cfg.L * (double(Z) * cfg.K * cfg.C + Z + double(cfg.C) * cfg.C) +
                cfg.C + double(cfg.H) * cfg.C;
            const double macs = per_sample * B;
            const double gmacs = macs / (ms * 1e6);
            // Block-parallel geometry: one workgroup per sample (B workgroups) of
            // WG=64 lanes; the dominant conv phase fills min(Z,WG) lanes each.
            // (Pre-fix this was a single workgroup of 256 lanes, B busy → 25% at
            // B=64 — see the git history of this file / gpu_compute.cpp.)
            const uint32_t WG = 64u;
            const uint32_t wg = B;
            const uint64_t busy = static_cast<uint64_t>(B) * std::min(Z, WG);
            rows.push_back({std::string("wavenet ") + cfg.name, "B=" + std::to_string(B),
                            macs, ms, gmacs,
                            occupancy_of(busy, wg, WG), wg, per_sample, 0});
        }
    }

    // ── Sibling one-thread-per-sample passes ─────────────────────────────────
    // additive_synth: one thread per sample, sums num_partials sin() terms.
    for (uint32_t P : {256u, 1024u}) {
        const uint32_t S = 512;
        std::vector<float> partials(static_cast<std::size_t>(P) * 3);
        for (uint32_t i = 0; i < P; ++i) {
            partials[i * 3 + 0] = 110.0f * (i + 1);      // freq
            partials[i * 3 + 1] = 1.0f / (i + 1);        // amp
            partials[i * 3 + 2] = 0.0f;                  // phase
        }
        std::vector<float> out(S, 0.0f);
        const double ms = median_ms(
            [&] { compute->additive_synth(partials.data(), out.data(), P, S,
                                          48000.0f, 0.0f); }, 30);
        const double macs = static_cast<double>(P) * S;  // ~1 MAC-equiv per partial-sample
        const double gmacs = macs / (ms * 1e6);
        const uint32_t wg = (S + 255u) / 256u;
        rows.push_back({"additive_synth", "P=" + std::to_string(P), macs, ms, gmacs,
                        occupancy_of(S, wg, 256), wg, static_cast<double>(P), 0});
    }

    // modal_strike: one thread per sample, sums num_modes damped sinusoids.
    for (uint32_t Mo : {64u, 256u}) {
        const uint32_t S = 512;
        std::vector<float> modes(static_cast<std::size_t>(Mo) * 4);
        for (uint32_t i = 0; i < Mo; ++i) {
            modes[i * 4 + 0] = 200.0f * (i + 1);
            modes[i * 4 + 1] = 1.0f / (i + 1);
            modes[i * 4 + 2] = 2.0f + 0.1f * i;
            modes[i * 4 + 3] = 0.0f;
        }
        std::vector<float> out(S, 0.0f);
        const double ms = median_ms(
            [&] { compute->modal_strike(modes.data(), out.data(), Mo, S,
                                        48000.0f, 0.0f); }, 30);
        const double macs = static_cast<double>(Mo) * S;
        const double gmacs = macs / (ms * 1e6);
        const uint32_t wg = (S + 255u) / 256u;
        rows.push_back({"modal_strike", "M=" + std::to_string(Mo), macs, ms, gmacs,
                        occupancy_of(S, wg, 256), wg, static_cast<double>(Mo), 0});
    }

    // ── Rank by (roofline gap × hotness) and print ───────────────────────────
    for (auto& r : rows) r.gap = r.gmacs > 0 ? peak_gmacs / r.gmacs : 0;
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return (a.gap * a.ms) > (b.gap * b.ms);
    });

    std::printf("\n%-26s %-9s %10s %8s %9s %6s %6s %9s %8s\n", "pass", "shape",
                "GMAC/s", "gap x", "block ms", "occ%", "wgs", "serialMAC", "rank");
    std::printf("%s\n", std::string(104, '-').c_str());
    int rank = 1;
    for (const auto& r : rows) {
        std::printf("%-26s %-9s %10.2f %8.0f %9.3f %6.0f %6u %9.0f %8d\n",
                    r.pass.c_str(), r.shape.c_str(), r.gmacs, r.gap, r.ms,
                    r.occupancy * 100.0, r.workgroups, r.serial_depth, rank++);
    }
    std::printf("\npeak (device roofline) = %.1f GMAC/s\n", peak_gmacs);
    return 0;
}
