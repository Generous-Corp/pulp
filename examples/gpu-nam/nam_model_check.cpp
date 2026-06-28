// Loads a real .nam capture, prints its parsed architecture, and validates the
// exact CPU WaveNet forward: weights consumed exactly, output finite + causal +
// non-trivial. Defaults to /tmp/test.nam; pass a path as argv[1] to override.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "nam_model.hpp"

using namespace pulp::examples::nam;

namespace {

bool all_finite(const std::vector<float>& v) {
    for (float x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "/tmp/test.nam";

    NamModel model;
    std::string err;
    if (!load_nam(path, model, &err)) {
        std::fprintf(stderr, "FAIL: load_nam(%s): %s\n", path.c_str(), err.c_str());
        return 1;
    }

    std::printf("=== NAM WaveNet: %s ===\n", path.c_str());
    std::printf("sample_rate     : %g Hz\n", model.sample_rate());
    std::printf("in_channels     : %d\n", model.in_channels());
    std::printf("out_channels    : %d\n", model.out_channels());
    std::printf("layer arrays    : %zu\n", model.arrays().size());
    for (std::size_t i = 0; i < model.arrays().size(); ++i) {
        const LayerArrayConfig& a = model.arrays()[i];
        std::printf("  array %zu: input=%d condition=%d channels=%d kernel=%d head=%d "
                    "head_bias=%d gated=%d act=%s dilations=[",
                    i, a.input_size, a.condition_size, a.channels, a.kernel_size, a.head_size,
                    a.head_bias ? 1 : 0, a.gated ? 1 : 0, a.activation.c_str());
        for (std::size_t d = 0; d < a.dilations.size(); ++d)
            std::printf("%s%d", d ? "," : "", a.dilations[d]);
        std::printf("]\n");
    }
    std::printf("head_scale      : %.9g (config) / %.9g (final weight)\n",
                model.head_scale_config(), model.head_scale());
    std::printf("weights         : provided=%zu  expected=%zu  consumed=%zu\n",
                model.weights_size(), model.expected_weight_count(), model.weights_consumed());

    const bool consumed_exact = model.weights_consumed() == model.weights_size()
                                && model.expected_weight_count() == model.weights_size();
    std::printf("weights consumed EXACTLY: %s\n", consumed_exact ? "YES" : "NO");

    // Run a test signal: a decaying sine with a unit impulse near the start.
    const std::uint32_t N = 2048;
    const double sr = model.sample_rate() > 0 ? model.sample_rate() : 48000.0;
    std::vector<float> in(N), out(N);
    for (std::uint32_t i = 0; i < N; ++i) {
        const double t = static_cast<double>(i) / sr;
        in[i] = static_cast<float>(0.5 * std::sin(2.0 * M_PI * 110.0 * t) * std::exp(-3.0 * t));
    }
    in[16] += 1.0f;   // impulse

    model.reset();
    model.process(in.data(), out.data(), N);

    const bool finite = all_finite(out);

    // Non-trivial: the output must actually vary (not all-zero / constant).
    float omin = out[0], omax = out[0], energy = 0.0f;
    for (float x : out) {
        omin = std::min(omin, x);
        omax = std::max(omax, x);
        energy += x * x;
    }
    const bool nontrivial = (omax - omin) > 1e-9f && energy > 1e-12f;

    // Causality: perturb input at sample T; outputs before T must be identical.
    const std::uint32_t T = 512;
    std::vector<float> in2 = in, out2(N);
    in2[T] += 0.37f;
    model.reset();
    model.process(in2.data(), out2.data(), N);
    bool causal = true;
    std::uint32_t first_divergence = N;
    for (std::uint32_t i = 0; i < N; ++i) {
        if (out[i] != out2[i]) {
            if (i < T) causal = false;
            if (first_divergence == N) first_divergence = i;
            if (i < T) break;
        }
    }

    std::printf("output finite   : %s\n", finite ? "YES" : "NO");
    std::printf("output range    : [%.6g, %.6g]  energy=%.6g\n", omin, omax, energy);
    std::printf("output non-trivial: %s\n", nontrivial ? "YES" : "NO");
    std::printf("causal (perturb@%u): %s  (first divergence at sample %u)\n",
                T, causal ? "YES" : "NO", first_divergence);

    const bool pass = consumed_exact && finite && nontrivial && causal;
    std::printf("\nRESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 2;
}
