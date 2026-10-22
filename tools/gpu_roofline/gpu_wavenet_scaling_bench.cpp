// Multi-instance WaveNet scaling bench for GpuCompute.
//
// Drives N independent WaveNet streams (one per plugin instance) through the
// real pulp::render::GpuCompute API, one block period at a time, and reports
// what a host thread that services every instance pays per period:
//   * wall time per period (mean / p50 / p99 / max) and the realtime ratio
//     (block period / mean wall) for the whole set of instances
//   * calling-thread CPU time per period (the blocking readback spins, so this
//     shows how much of the wait burns a core)
//   * GPU-busy time per period when the batched path is used and the device
//     exposes timestamp queries
//   * deadline misses (periods whose wall time exceeded the block period)
// and checks every measured stream against an independent double-precision
// streaming CPU reference (null depth in dB, finite output), plus two planted
// wrong controls that must fail the same check.
//
// Modes:
//   serial — one wavenet_forward() per instance per period (one submit and one
//            blocking readback each)
//   batch  — one wavenet_forward_batch() per period (all instances encoded into
//            one submit, read back through one map)
//   group  — one wavenet_forward_group() per period (all instances in shared
//            buffers, one dispatch per pass for the whole group)
//
// Tooling only; built under PULP_ENABLE_GPU. Needs a real GPU/Dawn device and
// exits 77 when none is available. Unpaced and short: it measures cost, not
// realtime reliability under a live audio callback.

#include "pulp/render/gpu_compute.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using pulp::render::GpuCompute;
using Clock = std::chrono::steady_clock;

namespace {

struct ArrayShape {
    uint32_t input_size, channels, head_size, kernel;
    bool head_bias;
    std::vector<uint32_t> dilations;
};

struct ModelShape {
    std::string name;
    std::vector<ArrayShape> arrays;
    float head_scale = 0.02f;
    std::vector<float> weights;  // empty -> synthesized per instance
    std::string provenance;
};

std::vector<uint32_t> pow2_dilations(uint32_t first, uint32_t last) {
    std::vector<uint32_t> d;
    for (uint32_t v = first; v <= last; v *= 2u) d.push_back(v);
    return d;
}

// NAM A1 presets (public architecture definitions), non-gated Tanh, kernel 3.
ModelShape a1_preset(const std::string& name) {
    ModelShape m;
    m.name = name;
    m.provenance = "synthetic weights, NAM A1 '" + name + "' shape";
    if (name == "standard") {
        m.arrays = {{1, 16, 8, 3, false, pow2_dilations(1, 512)},
                    {16, 8, 1, 3, true, pow2_dilations(1, 512)}};
        return m;
    }
    // Diagnostic: the standard shape and layer count with dilations capped at 8,
    // so the carried history is 16 columns instead of 1024. Separates per-layer
    // dispatch cost from history-size-proportional cost.
    // Diagnostic: the smallest legal net (one 1x1 layer, one channel). Its wall
    // time is the submit -> completion -> map round-trip floor.
    if (name == "trivial") {
        m.arrays = {{1, 1, 1, 1, false, {1}}};
        return m;
    }
    if (name == "standard-shortpad") {
        const std::vector<uint32_t> d = {1, 2, 4, 8, 1, 2, 4, 8, 1, 2};
        m.arrays = {{1, 16, 8, 3, false, d}, {16, 8, 1, 3, true, d}};
        return m;
    }
    uint32_t c0 = 0, c1 = 0;
    if (name == "lite") { c0 = 12; c1 = 6; }
    else if (name == "feather") { c0 = 8; c1 = 4; }
    else if (name == "nano") { c0 = 4; c1 = 2; }
    else return {};
    std::vector<uint32_t> d1 = {128, 256, 512};
    for (uint32_t v : pow2_dilations(1, 512)) d1.push_back(v);
    m.arrays = {{1, c0, c1, 3, false, pow2_dilations(1, 64)},
                {c0, c1, 1, 3, true, d1}};
    return m;
}

bool load_nam(const std::string& path, ModelShape& m) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        auto v = choc::json::parse(ss.str());
        if (v["architecture"].getString() != "WaveNet") return false;
        auto cfg = v["config"];
        auto layers = cfg["layers"];
        m.arrays.clear();
        for (uint32_t i = 0; i < layers.size(); ++i) {
            auto l = layers[i];
            if (l["gated"].getBool() || l["activation"].getString() != "Tanh") return false;
            if (l["condition_size"].getWithDefault<int64_t>(1) != 1) return false;
            ArrayShape a;
            a.input_size = static_cast<uint32_t>(l["input_size"].getInt64());
            a.channels = static_cast<uint32_t>(l["channels"].getInt64());
            a.head_size = static_cast<uint32_t>(l["head_size"].getInt64());
            a.kernel = static_cast<uint32_t>(l["kernel_size"].getInt64());
            a.head_bias = l["head_bias"].getBool();
            auto d = l["dilations"];
            for (uint32_t k = 0; k < d.size(); ++k)
                a.dilations.push_back(static_cast<uint32_t>(d[k].getInt64()));
            m.arrays.push_back(std::move(a));
        }
        auto w = v["weights"];
        m.weights.resize(w.size());
        for (uint32_t i = 0; i < w.size(); ++i)
            m.weights[i] = static_cast<float>(w[i].getFloat64());
        m.head_scale = m.weights.empty() ? 1.0f : m.weights.back();
        const auto slash = path.find_last_of('/');
        m.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        m.provenance = "file " + path;
        return true;
    } catch (...) {
        return false;
    }
}

// Total blob length in prepare_wavenet's layout, including the trailing head_scale.
uint32_t blob_len(const ModelShape& m) {
    uint64_t n = 0;
    for (const auto& a : m.arrays) {
        const uint64_t C = a.channels, Z = C;
        n += C * a.input_size;
        n += a.dilations.size() * (Z * C * a.kernel + Z + Z + C * C + C);
        n += a.head_size * C + (a.head_bias ? a.head_size : 0u);
    }
    return static_cast<uint32_t>(n + 1);
}

std::vector<float> synth_weights(const ModelShape& m, uint32_t seed) {
    std::vector<float> w(blob_len(m));
    uint32_t r = 0x9e3779b9u ^ (seed * 2654435761u);
    for (auto& x : w) {
        r = r * 1664525u + 1013904223u;
        x = (static_cast<float>(r >> 8) / 16777216.0f - 0.5f) * 0.2f;
    }
    w.back() = m.head_scale;
    return w;
}

std::vector<float> make_input(std::size_t n, uint32_t seed) {
    std::vector<float> s(n);
    uint32_t r = 0x1234567u + seed * 7919u;
    const float f1 = 0.031f + 0.0007f * static_cast<float>(seed % 17u);
    for (std::size_t i = 0; i < n; ++i) {
        r = r * 1664525u + 1013904223u;
        const float noise = static_cast<float>(r >> 9) / 4194304.0f - 1.0f;
        s[i] = 0.5f * std::sin(f1 * static_cast<float>(i)) +
               0.2f * std::sin(0.11f * static_cast<float>(i)) + 0.05f * noise;
    }
    return s;
}

// Independent double-precision streaming reference of the documented WaveNet
// (rechannel -> dilated layers with input mixin, Tanh, residual and head
// accumulate -> head rechannel, chained array to array, then head_scale).
std::vector<double> cpu_reference(const ModelShape& m, const std::vector<float>& w,
                                  const std::vector<float>& x) {
    const std::size_t T = x.size();
    std::vector<double> cur(T);  // array input, input_size channels, sample-major
    for (std::size_t t = 0; t < T; ++t) cur[t] = x[t];
    uint32_t cur_ch = 1;
    std::vector<double> head_in;  // previous array's head output (head_size == channels)
    std::size_t off = 0;
    std::vector<double> out(T, 0.0);
    for (std::size_t ai = 0; ai < m.arrays.size(); ++ai) {
        const auto& a = m.arrays[ai];
        const uint32_t C = a.channels, K = a.kernel;
        std::vector<double> act(T * C, 0.0);
        for (std::size_t t = 0; t < T; ++t)
            for (uint32_t oc = 0; oc < C; ++oc) {
                double acc = 0.0;
                for (uint32_t ic = 0; ic < cur_ch; ++ic)
                    acc += w[off + oc * cur_ch + ic] * cur[t * cur_ch + ic];
                act[t * C + oc] = acc;
            }
        off += static_cast<std::size_t>(C) * cur_ch;
        std::vector<double> head(T * C, 0.0);
        if (ai > 0) head = head_in;
        std::vector<double> next(T * C), z(C), av(C);
        for (uint32_t dil : a.dilations) {
            const std::size_t cw = off, cb = cw + C * C * K, mw = cb + C, lw = mw + C, lb = lw + C * C;
            for (std::size_t t = 0; t < T; ++t) {
                for (uint32_t oc = 0; oc < C; ++oc) {
                    double acc = w[cb + oc];
                    for (uint32_t k = 0; k < K; ++k) {
                        const std::size_t back = static_cast<std::size_t>(dil) * (K - 1u - k);
                        if (t < back) continue;  // zero history before the stream
                        for (uint32_t ic = 0; ic < C; ++ic)
                            acc += w[cw + oc * C * K + ic * K + k] * act[(t - back) * C + ic];
                    }
                    acc += w[mw + oc] * x[t];
                    z[oc] = acc;
                }
                for (uint32_t c = 0; c < C; ++c) {
                    av[c] = std::tanh(z[c]);
                    head[t * C + c] += av[c];
                }
                for (uint32_t oc = 0; oc < C; ++oc) {
                    double r = w[lb + oc];
                    for (uint32_t ic = 0; ic < C; ++ic) r += w[lw + oc * C + ic] * av[ic];
                    next[t * C + oc] = act[t * C + oc] + r;
                }
            }
            act.swap(next);
            off = lb + C;
        }
        const uint32_t H = a.head_size;
        std::vector<double> hout(T * H);
        for (std::size_t t = 0; t < T; ++t)
            for (uint32_t oc = 0; oc < H; ++oc) {
                double acc = a.head_bias ? w[off + H * C + oc] : 0.0;
                for (uint32_t ic = 0; ic < C; ++ic) acc += w[off + oc * C + ic] * head[t * C + ic];
                hout[t * H + oc] = acc;
            }
        off += static_cast<std::size_t>(H) * C + (a.head_bias ? H : 0u);
        head_in = hout;
        cur = act;
        cur_ch = C;
        if (ai + 1 == m.arrays.size())
            for (std::size_t t = 0; t < T; ++t) out[t] = hout[t * H] * w.back();
    }
    return out;
}

double null_db(const std::vector<float>& got, const std::vector<double>& ref) {
    double e = 0.0, s = 0.0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const double d = static_cast<double>(got[i]) - ref[i];
        e += d * d;
        s += ref[i] * ref[i];
    }
    if (s <= 0.0) return 0.0;
    return e <= 0.0 ? -300.0 : 10.0 * std::log10(e / s);
}

bool all_finite(const std::vector<float>& v) {
    for (float f : v)
        if (!std::isfinite(f)) return false;
    return true;
}

double thread_cpu_us() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) / 1e3;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(p * (v.size() - 1) + 0.5));
    return v[i];
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

struct Plan {
    std::vector<GpuCompute::WavenetLayerArraySpec> specs;
};

Plan make_specs(const ModelShape& m) {
    Plan p;
    for (const auto& a : m.arrays) {
        GpuCompute::WavenetLayerArraySpec s;
        s.input_size = a.input_size;
        s.condition_size = 1;
        s.channels = a.channels;
        s.kernel = a.kernel;
        s.head_size = a.head_size;
        s.gated = 0;
        s.head_bias = a.head_bias ? 1u : 0u;
        s.dilations = a.dilations.data();
        s.num_layers = static_cast<uint32_t>(a.dilations.size());
        p.specs.push_back(s);
    }
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> models = {"nano", "feather", "lite", "standard"};
    std::vector<uint32_t> blocks = {32, 64, 128, 256, 512};
    std::vector<uint32_t> counts = {1, 4, 8, 16, 32, 64, 100};
    std::string mode = "serial";
    std::string json_path;
    double seconds = 1.0;
    uint32_t check_instances = 4;
    double sample_rate = 48000.0;
    bool keep_warm = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--models") models = split(v);
        else if (k == "--blocks") { blocks.clear(); for (auto& s : split(v)) blocks.push_back(std::stoul(s)); }
        else if (k == "--instances") { counts.clear(); for (auto& s : split(v)) counts.push_back(std::stoul(s)); }
        else if (k == "--mode") mode = v;
        else if (k == "--seconds") seconds = std::stod(v);
        else if (k == "--check-instances") check_instances = static_cast<uint32_t>(std::stoul(v));
        else if (k == "--json") json_path = v;
        else if (k == "--keep-warm") keep_warm = v == "1";
        else { std::fprintf(stderr, "unknown argument %s\n", k.c_str()); return 2; }
    }
    if (mode != "serial" && mode != "batch" && mode != "group") {
        std::fprintf(stderr, "--mode serial|batch|group\n");
        return 2;
    }

    auto probe = GpuCompute::create();
    if (!probe || !probe->initialize_standalone()) { std::printf("SKIP: no GPU compute device\n"); return 77; }
    probe.reset();

    // Optional background load: a second device continuously running trivial
    // forwards, so the GPU never idles between the measured submissions.
    std::atomic<bool> stop_warm{false};
    std::thread warm_thread;
    if (keep_warm) {
        warm_thread = std::thread([&] {
            auto dev = GpuCompute::create();
            if (!dev || !dev->initialize_standalone()) return;
            ModelShape t = a1_preset("trivial");
            Plan tp = make_specs(t);
            auto w = synth_weights(t, 0);
            if (!dev->prepare_wavenet(tp.specs.data(), 1, w.data(), blob_len(t), 32, w.back(), 0)) return;
            std::vector<float> a(32, 0.1f), b(32);
            while (!stop_warm.load()) dev->wavenet_forward(a.data(), b.data(), 32, 0);
        });
    }

    std::string json = "[\n";
    bool first_row = true;
    int failures = 0;
    std::printf("%-26s %5s %4s %6s %10s %10s %10s %10s %10s %10s %8s %6s %9s\n", "model", "block",
                "N", "mode", "mean_us", "p50_us", "p99_us", "min_us", "cpu_us", "gpu_us",
                "rt_ratio", "miss", "null_db");

    for (const auto& mname : models) {
        ModelShape m;
        if (mname.find(".nam") != std::string::npos) {
            if (!load_nam(mname, m)) { std::fprintf(stderr, "cannot load %s\n", mname.c_str()); return 2; }
        } else {
            m = a1_preset(mname);
            if (m.arrays.empty()) { std::fprintf(stderr, "unknown model %s\n", mname.c_str()); return 2; }
        }
        const Plan plan = make_specs(m);
        const uint32_t need = blob_len(m);
        if (!m.weights.empty() && m.weights.size() != need) {
            std::fprintf(stderr, "%s: weight count %zu != expected %u\n", m.name.c_str(),
                         m.weights.size(), need);
            return 2;
        }
        for (uint32_t B : blocks) {
            for (uint32_t N : counts) {
                // A fresh device per row keeps plan memory bounded and history clean.
                auto gpu = GpuCompute::create();
                if (!gpu || !gpu->initialize_standalone()) { std::printf("SKIP: no GPU compute device\n"); return 77; }
                std::vector<std::vector<float>> weights(N);
                bool ok = true;
                std::vector<const float*> wp(N);
                std::vector<float> scales(N);
                for (uint32_t i = 0; i < N && ok; ++i) {
                    weights[i] = m.weights.empty() ? synth_weights(m, i) : m.weights;
                    wp[i] = weights[i].data();
                    scales[i] = weights[i].back();
                    if (mode != "group")
                        ok = gpu->prepare_wavenet(plan.specs.data(), static_cast<uint32_t>(plan.specs.size()),
                                                  weights[i].data(), need, B, weights[i].back(), i);
                }
                if (ok && mode == "group")
                    ok = gpu->prepare_wavenet_group(plan.specs.data(), static_cast<uint32_t>(plan.specs.size()),
                                                    wp.data(), need, scales.data(), N, B, 0);
                if (!ok) { std::printf("%s B=%u N=%u: prepare_wavenet failed\n", m.name.c_str(), B, N); ++failures; continue; }

                const double period_us = 1e6 * B / sample_rate;
                const uint32_t warm = 32;
                const uint32_t periods = std::max<uint32_t>(64, static_cast<uint32_t>(seconds * sample_rate / B));
                const uint32_t check = std::min(N, check_instances);
                // Correctness covers the warmup plus at least 4096 samples.
                const uint32_t check_periods = std::max<uint32_t>(warm, (4096u + B - 1u) / B);
                const uint32_t total = warm + std::max(periods, check_periods);
                std::vector<std::vector<float>> in(N), out(N);
                for (uint32_t i = 0; i < N; ++i) {
                    in[i] = make_input(static_cast<std::size_t>(total) * B, i);
                    out[i].assign(static_cast<std::size_t>(total) * B, 0.0f);
                }
                std::vector<const float*> ip(N);
                std::vector<float*> op(N);
                std::vector<uint32_t> ids(N);
                for (uint32_t i = 0; i < N; ++i) ids[i] = i;
                std::vector<double> wall, cpu, gpu_busy;
                wall.reserve(total);
                // Stop early once the minimum period count and the time budget are
                // both met; very slow rows would otherwise run for minutes.
                const uint32_t min_total = warm + std::max<uint32_t>(64, check_periods);
                const auto budget_start = Clock::now();
                uint32_t ran = 0;
                for (uint32_t p = 0; p < total && ok; ++p) {
                    if (p >= min_total &&
                        std::chrono::duration<double>(Clock::now() - budget_start).count() >= seconds)
                        break;
                    ran = p + 1;
                    for (uint32_t i = 0; i < N; ++i) {
                        ip[i] = in[i].data() + static_cast<std::size_t>(p) * B;
                        op[i] = out[i].data() + static_cast<std::size_t>(p) * B;
                    }
                    // Every 8th batched period carries a GPU timestamp; its second
                    // readback would inflate wall time, so it is kept out of the
                    // wall/CPU statistics.
                    const bool timed = mode != "serial" && p % 8u == 7u;
                    double g = -1.0;
                    const double c0 = thread_cpu_us();
                    const auto t0 = Clock::now();
                    if (mode == "serial") {
                        for (uint32_t i = 0; i < N && ok; ++i) ok = gpu->wavenet_forward(ip[i], op[i], B, i);
                    } else if (mode == "batch") {
                        ok = gpu->wavenet_forward_batch(ip.data(), op.data(), ids.data(), N, B, timed ? &g : nullptr);
                    } else {
                        ok = gpu->wavenet_forward_group(ip.data(), op.data(), N, B, 0, timed ? &g : nullptr);
                    }
                    const auto t1 = Clock::now();
                    const double c1 = thread_cpu_us();
                    if (p >= warm) {
                        if (g >= 0.0) gpu_busy.push_back(g);
                        if (!timed) {
                            wall.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
                            cpu.push_back(c1 - c0);
                        }
                    }
                }
                if (!ok) { std::printf("%s B=%u N=%u: forward failed\n", m.name.c_str(), B, N); ++failures; continue; }
                (void)ran;

                // Correctness: every checked instance plus the last one.
                double worst = -300.0;
                bool finite = true;
                std::vector<uint32_t> checked;
                for (uint32_t i = 0; i < check; ++i) checked.push_back(i);
                if (N > check) checked.push_back(N - 1);
                const std::size_t ns = static_cast<std::size_t>(check_periods) * B;
                for (uint32_t i : checked) {
                    std::vector<float> x(in[i].begin(), in[i].begin() + static_cast<std::ptrdiff_t>(ns));
                    std::vector<float> y(out[i].begin(), out[i].begin() + static_cast<std::ptrdiff_t>(ns));
                    finite = finite && all_finite(y);
                    worst = std::max(worst, null_db(y, cpu_reference(m, weights[i], x)));
                }
                // Planted-wrong controls on instance 0, scored by the same null
                // measure: the reference driven by another stream's input, and the
                // reference with head_scale off by 0.1% (about -60 dB). Both must
                // land above the -80 dB pass line, proving the gate can see them.
                double ctl_swap = 0.0, ctl_gain = 0.0;
                {
                    std::vector<float> y0(out[0].begin(), out[0].begin() + static_cast<std::ptrdiff_t>(ns));
                    std::vector<float> xo = make_input(ns, 1000u);
                    ctl_swap = null_db(y0, cpu_reference(m, weights[0], xo));
                    std::vector<float> x0(in[0].begin(), in[0].begin() + static_cast<std::ptrdiff_t>(ns));
                    auto wrong = weights[0];
                    wrong.back() *= 1.001f;
                    ctl_gain = null_db(y0, cpu_reference(m, wrong, x0));
                }
                const bool pass = finite && worst < -80.0 && ctl_swap > -80.0 && ctl_gain > -80.0;
                if (!pass) ++failures;

                double mean = 0.0;
                for (double v : wall) mean += v;
                mean /= static_cast<double>(wall.size());
                double cmean = 0.0;
                for (double v : cpu) cmean += v;
                cmean /= static_cast<double>(cpu.size());
                double gmean = -1.0;
                if (!gpu_busy.empty()) {
                    gmean = 0.0;
                    for (double v : gpu_busy) gmean += v;
                    gmean /= static_cast<double>(gpu_busy.size());
                }
                uint32_t misses = 0;
                for (double v : wall) misses += v > period_us ? 1u : 0u;
                const double p50 = pct(wall, 0.50), p99 = pct(wall, 0.99), mx = pct(wall, 1.0),
                             mn = pct(wall, 0.0);
                std::printf("%-26s %5u %4u %6s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %8.2f %6u %9.1f%s\n",
                            m.name.c_str(), B, N, mode.c_str(), mean, p50, p99, mn, cmean, gmean,
                            period_us / mean, misses, worst, pass ? "" : "  FAIL");
                if (!pass)
                    std::printf("    finite=%d swap_control=%.1f dB gain_control=%.1f dB\n", finite ? 1 : 0,
                                ctl_swap, ctl_gain);
                char buf[1024];
                std::snprintf(buf, sizeof(buf),
                              "%s  {\"model\":\"%s\",\"provenance\":\"%s\",\"block\":%u,\"instances\":%u,"
                              "\"mode\":\"%s\",\"periods\":%zu,\"period_us\":%.3f,\"mean_us\":%.3f,"
                              "\"p50_us\":%.3f,\"p99_us\":%.3f,\"max_us\":%.3f,\"min_us\":%.3f,\"cpu_us\":%.3f,"
                              "\"gpu_busy_us\":%.3f,\"rt_ratio\":%.4f,\"misses\":%u,\"null_db_worst\":%.2f,"
                              "\"control_swap_db\":%.2f,\"control_gain_db\":%.2f,\"finite\":%s,\"pass\":%s}",
                              first_row ? "" : ",\n", m.name.c_str(), m.provenance.c_str(), B, N,
                              mode.c_str(), wall.size(), period_us, mean, p50, p99, mx, mn, cmean, gmean,
                              period_us / mean, misses, worst, ctl_swap, ctl_gain,
                              finite ? "true" : "false", pass ? "true" : "false");
                json += buf;
                first_row = false;
            }
        }
    }
    stop_warm = true;
    if (warm_thread.joinable()) warm_thread.join();
    json += "\n]\n";
    if (!json_path.empty()) {
        std::ofstream f(json_path);
        f << json;
    }
    return failures == 0 ? 0 : 1;
}
