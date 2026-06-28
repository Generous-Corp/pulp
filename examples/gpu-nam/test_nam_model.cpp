// Tests for the exact NAM WaveNet CPU inference + .nam loader.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

#include "nam_model.hpp"

using namespace pulp::examples::nam;
using Catch::Matchers::WithinAbs;

namespace {

bool file_exists(const char* p) {
    std::ifstream f(p);
    return f.good();
}

} // namespace

TEST_CASE("synthetic single-layer model matches hand-computed forward", "[nam]") {
    // One array, one layer, all 1x1, channels=1 — so the whole forward reduces to
    // a closed-form expression we can check against, exercising the exact weight
    // order: rechannel, conv(+bias), input_mixin, layer1x1(+bias),
    // head_rechannel(+bias), head_scale.
    LayerArrayConfig a;
    a.input_size = 1;
    a.condition_size = 1;
    a.channels = 1;
    a.kernel_size = 1;
    a.dilations = {1};
    a.head_size = 1;
    a.head_bias = true;
    a.gated = false;
    a.activation = "Tanh";

    const float w_re = 0.8f;   // rechannel
    const float wc = 1.3f, bc = -0.2f;   // conv weight + bias
    const float wm = 0.5f;     // input mixin
    const float wl = 0.7f, bl = 0.1f;    // layer1x1 weight + bias
    const float wh = 1.1f, bh = -0.05f;  // head rechannel weight + bias
    const float hs = 0.25f;    // head_scale (final weight)

    std::vector<float> weights = {w_re, wc, bc, wm, wl, bl, wh, bh, hs};

    NamModel model;
    REQUIRE(model.build({a}, hs, weights, /*in_channels=*/1, /*sample_rate=*/48000.0));
    REQUIRE(model.weights_consumed() == weights.size());
    REQUIRE(model.expected_weight_count() == weights.size());
    REQUIRE(model.head_scale() == hs);

    auto expected = [&](float x) {
        const float rc = w_re * x;
        const float z = wc * rc + bc + wm * x;
        const float act = std::tanh(z);
        const float head = wh * act + bh;
        return hs * head;
    };

    model.reset();
    for (float x : {-1.0f, -0.3f, 0.0f, 0.42f, 0.9f, 2.5f}) {
        const float y = model.process_sample(x);
        REQUIRE_THAT(y, WithinAbs(expected(x), 1e-6f));
    }
}

TEST_CASE("weight count is computed from the exact layout", "[nam]") {
    // Two arrays mirroring the published example model's shape; the expected
    // float count must match the closed-form layout sum.
    LayerArrayConfig a0;
    a0.input_size = 1; a0.condition_size = 1; a0.channels = 3; a0.kernel_size = 3;
    a0.dilations = {1, 2}; a0.head_size = 2; a0.head_bias = false; a0.gated = false;
    LayerArrayConfig a1;
    a1.input_size = 3; a1.condition_size = 1; a1.channels = 2; a1.kernel_size = 3;
    a1.dilations = {8}; a1.head_size = 1; a1.head_bias = true; a1.gated = false;

    // array0: rechannel 1*3 + 2 layers*(conv 3*3*3+3, mixin 1*3, layer1x1 3*3+3)
    //         + head_rechannel 3*2  = 3 + 2*45 + 6 = 99
    // array1: rechannel 3*2 + 1 layer*(conv 2*2*3+2, mixin 1*2, layer1x1 2*2+2)
    //         + head_rechannel 2*1+1 = 6 + 22 + 3 = 31
    // + head_scale = 99 + 31 + 1 = 131
    const std::size_t expected = 131;
    std::vector<float> weights(expected, 0.01f);

    NamModel model;
    REQUIRE(model.build({a0, a1}, 0.5f, weights, 1, 48000.0));
    REQUIRE(model.expected_weight_count() == expected);
    REQUIRE(model.weights_consumed() == expected);
}

TEST_CASE("build rejects a short/long weight blob", "[nam]") {
    LayerArrayConfig a;
    a.input_size = 1; a.condition_size = 1; a.channels = 1; a.kernel_size = 1;
    a.dilations = {1}; a.head_size = 1; a.head_bias = true;

    NamModel too_short;
    REQUIRE_FALSE(too_short.build({a}, 0.25f, std::vector<float>(8, 0.0f), 1, 48000.0));
    REQUIRE_FALSE(too_short.error().empty());

    NamModel too_long;
    REQUIRE_FALSE(too_long.build({a}, 0.25f, std::vector<float>(10, 0.0f), 1, 48000.0));
}

TEST_CASE("loader parses the real example .nam exactly", "[nam]") {
    const char* path = "/tmp/test.nam";
    if (!file_exists(path)) {
        WARN("/tmp/test.nam not present; skipping real-model load test");
        return;
    }

    NamModel model;
    std::string err;
    REQUIRE(load_nam(path, model, &err));
    INFO("load error: " << err);

    // Architecture sanity for the published example model.
    REQUIRE(model.arrays().size() == 2);
    REQUIRE(model.out_channels() == 1);
    REQUIRE(model.in_channels() == 1);

    // Weights consumed byte-for-byte: none left over, none missing.
    REQUIRE(model.weights_consumed() == model.weights_size());
    REQUIRE(model.expected_weight_count() == model.weights_size());

    // head_scale stored as the final weight should equal the config value.
    REQUIRE_THAT(model.head_scale(), WithinAbs(model.head_scale_config(), 1e-6f));
}

TEST_CASE("real model output is finite, non-trivial, and causal", "[nam]") {
    const char* path = "/tmp/test.nam";
    if (!file_exists(path)) {
        WARN("/tmp/test.nam not present; skipping real-model inference test");
        return;
    }

    NamModel model;
    REQUIRE(load_nam(path, model));

    const std::uint32_t N = 1024;
    std::vector<float> in(N), out(N);
    for (std::uint32_t i = 0; i < N; ++i)
        in[i] = 0.3f * std::sin(0.05f * static_cast<float>(i));
    in[8] += 1.0f;   // impulse

    model.reset();
    model.process(in.data(), out.data(), N);

    bool finite = true, nontrivial = false;
    for (float x : out) {
        if (!std::isfinite(x)) finite = false;
        if (std::fabs(x) > 1e-9f) nontrivial = true;
    }
    REQUIRE(finite);
    REQUIRE(nontrivial);

    // Causality: perturbing input at sample T must not change any output < T.
    const std::uint32_t T = 256;
    std::vector<float> in2 = in, out2(N);
    in2[T] += 0.5f;
    model.reset();
    model.process(in2.data(), out2.data(), N);

    for (std::uint32_t i = 0; i < T; ++i)
        REQUIRE(out[i] == out2[i]);
    // And the perturbation must actually have an effect at/after T.
    bool diverged = false;
    for (std::uint32_t i = T; i < N; ++i)
        if (out[i] != out2[i]) { diverged = true; break; }
    REQUIRE(diverged);
}
