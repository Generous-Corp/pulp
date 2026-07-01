// Tests for the NAM Architecture 2 (A2) CPU inference, its .nam loader
// (SlimmableContainer + A2-shaped WaveNet), and the NamRuntime dispatch.
//
// A2 is a WaveNet variant: dilated causal conv stack with per-layer kernel sizes,
// per-layer LeakyReLU, a windowed conv head, and a packed SlimmableContainer of
// size variants. The goldens here are computed by hand from that definition, on
// deliberately tiny models where the arithmetic is tractable:
//
//   output = head_scale * ( head_conv( skip_accumulator ) )
//   skip   = LeakyReLU( conv(rechannel(x)) + input_mixin(condition) )
//
// With one layer, kernel_size 1, condition weight 0 and a memoryless head this
// reduces to a closed form; a second case uses a kernel-2 head with equal taps to
// exercise the windowed (time-domain) head without depending on tap order.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nam_a2.hpp"
#include "nam_model.hpp"
#include "nam_runtime.hpp"

using namespace pulp::examples::nam;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

std::string write_temp(const std::string& name, const std::string& content) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f << content;
    f.close();
    return p.string();
}

std::string floats_json(const std::vector<float>& w) {
    std::string s = "[";
    for (std::size_t i = 0; i < w.size(); ++i) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.8g", w[i]);
        s += buf;
        if (i + 1 < w.size()) s += ",";
    }
    return s + "]";
}

// One A2 WaveNet submodel object: single layer array, per-layer LeakyReLU, a
// windowed conv head. Only the fields the loader reads are emitted (absent knobs
// are treated as inactive), plus the flat weights (trailing scalar = head_scale).
std::string a2_submodel(int channels, const std::vector<int>& kernel_sizes,
                        const std::vector<int>& dilations, float slope, int head_kernel,
                        const std::vector<float>& weights) {
    std::string ks = "[";
    for (std::size_t i = 0; i < kernel_sizes.size(); ++i)
        ks += std::to_string(kernel_sizes[i]) + (i + 1 < kernel_sizes.size() ? "," : "");
    ks += "]";
    std::string dl = "[";
    for (std::size_t i = 0; i < dilations.size(); ++i)
        dl += std::to_string(dilations[i]) + (i + 1 < dilations.size() ? "," : "");
    dl += "]";
    std::string act = "[";
    char sb[40];
    std::snprintf(sb, sizeof(sb), "%.8g", slope);
    for (std::size_t i = 0; i < dilations.size(); ++i)
        act += std::string("{\"type\":\"LeakyReLU\",\"negative_slope\":") + sb + "}"
               + (i + 1 < dilations.size() ? "," : "");
    act += "]";
    return std::string("{\"architecture\":\"WaveNet\",\"sample_rate\":48000,\"config\":{\"layers\":[{")
           + "\"input_size\":1,\"condition_size\":1,\"channels\":" + std::to_string(channels)
           + ",\"bottleneck\":" + std::to_string(channels)
           + ",\"kernel_sizes\":" + ks + ",\"dilations\":" + dl + ",\"activation\":" + act
           + ",\"head\":{\"out_channels\":1,\"kernel_size\":" + std::to_string(head_kernel)
           + ",\"bias\":true}}],\"head_scale\":1.0},\"weights\":" + floats_json(weights) + "}";
}

std::string a2_container(const std::vector<std::pair<double, std::string>>& submodels) {
    std::string subs = "[";
    for (std::size_t i = 0; i < submodels.size(); ++i) {
        char mv[40];
        std::snprintf(mv, sizeof(mv), "%.8g", submodels[i].first);
        subs += std::string("{\"max_value\":") + mv + ",\"model\":" + submodels[i].second + "}"
                + (i + 1 < submodels.size() ? "," : "");
    }
    subs += "]";
    return std::string("{\"architecture\":\"SlimmableContainer\",\"sample_rate\":48000,")
           + "\"config\":{\"submodels\":" + subs + "},\"weights\":[]}";
}

}  // namespace

TEST_CASE("A2 single-layer forward matches a hand-computed closed form", "[nam][a2]") {
    // channels=1, one layer kernel=1, head kernel=1. Weights (consumption order):
    //   rechannel[w_rc], conv[w_c, b_c], input_mixin[w_m], layer1x1[w_l, b_l],
    //   head[w_h, b_h], head_scale.
    // output = head_scale * ( w_h * LeakyReLU(w_c*w_rc*x + w_m*x + b_c) + b_h ).
    // With w_rc=1,w_c=2,b_c=0.5,w_m=1,w_h=1,b_h=0,head_scale=0.5, slope=0.1:
    //   output = 0.5 * LeakyReLU(3x + 0.5).
    const std::vector<float> w = {1.0f, 2.0f, 0.5f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f};
    NamA2 m;
    std::string err;
    const std::string json = a2_submodel(1, {1}, {1}, /*slope=*/0.1f, /*head_kernel=*/1, w);
    const std::string path = write_temp("gpu_nam_a2_single.nam", json);
    REQUIRE(load_nam_a2(path, m, &err));
    REQUIRE(m.ok());
    m.reset();
    // x=1 -> 0.5*3.5 = 1.75 ; x=-1 -> 0.5*(0.1*-2.5) = -0.125 ; x=0 -> 0.5*0.5 = 0.25.
    CHECK_THAT(m.process_sample(1.0f), WithinAbs(1.75f, 1e-6));
    CHECK_THAT(m.process_sample(-1.0f), WithinAbs(-0.125f, 1e-6));
    CHECK_THAT(m.process_sample(0.0f), WithinAbs(0.25f, 1e-6));
    std::filesystem::remove(path);
}

TEST_CASE("A2 windowed head (kernel 2) convolves causally over time", "[nam][a2]") {
    // Identity layer (slope=1 makes LeakyReLU the identity, condition weight 0), so
    // the skip a[n] == x[n]. Head kernel=2 with ASYMMETRIC taps so the test pins the
    // causal tap order (a reversed order or off-by-one would change the result):
    // the .nam head weights are [k0, k1] and the conv maps k0 -> oldest sample, so
    //   output[n] = 2*a[n-1] + 3*a[n] = 2*x[n-1] + 3*x[n].
    // Weights: rechannel[1], conv[1,0], input_mixin[0], layer1x1[1,0],
    //          head[k0=2, k1=3, bias=0], head_scale[1].
    const std::vector<float> w = {1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 2.0f, 3.0f, 0.0f, 1.0f};
    NamA2 m;
    std::string err;
    const std::string json = a2_submodel(1, {1}, {1}, /*slope=*/1.0f, /*head_kernel=*/2, w);
    const std::string path = write_temp("gpu_nam_a2_head2.nam", json);
    REQUIRE(load_nam_a2(path, m, &err));
    m.reset();
    const std::vector<float> in = {1.0f, 0.0f, 0.0f, 2.0f, -1.0f};
    // n0: 2*0 + 3*1 = 3 ; n1: 2*1 + 3*0 = 2 ; n2: 0 ; n3: 2*0 + 3*2 = 6 ; n4: 2*2 + 3*-1 = 1.
    const std::vector<float> golden = {3.0f, 2.0f, 0.0f, 6.0f, 1.0f};
    for (std::size_t i = 0; i < in.size(); ++i)
        CHECK_THAT(m.process_sample(in[i]), WithinAbs(golden[i], 1e-6));
    std::filesystem::remove(path);
}

TEST_CASE("A2 SlimmableContainer defaults to full, set_size selects a variant", "[nam][a2]") {
    // Two memoryless variants (identity activation, kernel=1): Lite -> 2x, Full -> 3x.
    // Weights: rechannel[1], conv[gain,0], input_mixin[0], layer1x1[1,0], head[1,0], scale[1].
    const std::string lite = a2_submodel(1, {1}, {1}, 1.0f, 1,
                                         {1.0f, 2.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f});
    const std::string full = a2_submodel(1, {1}, {1}, 1.0f, 1,
                                         {1.0f, 3.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f});
    const std::string json = a2_container({{0.5, lite}, {1.0, full}});
    const std::string path = write_temp("gpu_nam_a2_container.nam", json);
    NamA2 m;
    std::string err;
    REQUIRE(load_nam_a2(path, m, &err));
    REQUIRE(m.variant_count() == 2);

    m.reset();
    CHECK_THAT(m.process_sample(1.0f), WithinAbs(3.0f, 1e-6));   // default = full (largest)

    CHECK(m.set_size(0.3));                                      // -> Lite (max_value 0.5)
    CHECK_THAT(m.process_sample(1.0f), WithinAbs(2.0f, 1e-6));
    CHECK(m.set_size(0.8));                                      // -> Full (max_value 1.0)
    CHECK_THAT(m.process_sample(1.0f), WithinAbs(3.0f, 1e-6));
    CHECK_FALSE(m.set_size(0.9));                                // still Full, no change
    std::filesystem::remove(path);
}

TEST_CASE("A2 is_nam_a2 classifier separates A2 from A1", "[nam][a2]") {
    // A2-shaped WaveNet (kernel_sizes array + head dict) and a container are A2.
    const std::string a2wave = a2_submodel(1, {1}, {1}, 0.1f, 2,
                                            {1, 1, 0, 0, 1, 0, 1, 1, 0, 1});
    const std::string container = a2_container({{1.0, a2wave}});
    // An A1 WaveNet: scalar kernel_size, string activation, no head dict.
    const std::string a1 =
        "{\"architecture\":\"WaveNet\",\"config\":{\"layers\":[{\"input_size\":1,"
        "\"condition_size\":1,\"channels\":1,\"kernel_size\":1,\"head_size\":1,"
        "\"activation\":\"Tanh\",\"dilations\":[1]}],\"head_scale\":1.0},\"weights\":[]}";

    CHECK(is_nam_a2(choc::json::parse(a2wave)));
    CHECK(is_nam_a2(choc::json::parse(container)));
    CHECK_FALSE(is_nam_a2(choc::json::parse(a1)));
}

TEST_CASE("A2 loads through NamRuntime dispatch", "[nam][a2][runtime]") {
    const std::vector<float> w = {1.0f, 2.0f, 0.5f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f};
    const std::string json = a2_submodel(1, {1}, {1}, 0.1f, 1, w);
    const std::string path = write_temp("gpu_nam_a2_runtime.nam", json);
    NamRuntime rt;
    std::string err;
    REQUIRE(load_nam_runtime(path, rt, &err));
    REQUIRE(rt.ok());
    CHECK(rt.arch() == NamRuntime::Arch::WaveNetA2);
    CHECK(std::string(rt.arch_name()) == "WaveNet-A2");
    CHECK_FALSE(rt.gpu_eligible());     // A2's GPU forward is not wired yet -> CPU
    CHECK(rt.sample_rate() == 48000.0);
    rt.reset();
    CHECK_THAT(rt.process_sample(1.0f), WithinAbs(1.75f, 1e-6));
    CHECK_THAT(rt.process_sample(-1.0f), WithinAbs(-0.125f, 1e-6));
    std::filesystem::remove(path);
}

TEST_CASE("A2 rejects unsupported shapes with the right error, not a stray one", "[nam][a2]") {
    // Returns the load error (or "<loaded ok>"). Each case asserts the SPECIFIC guard
    // fires — otherwise deleting a guard could still "pass" via a later weight-count
    // failure. The base weights are shape-correct for a channels-1/head-kernel-1 model,
    // so a structural guard is what actually trips.
    auto err_for = [](const std::string& json) {
        NamA2 m;
        std::string err;
        return m.build(choc::json::parse(json), &err) ? std::string("<loaded ok>") : err;
    };
    const std::vector<float> w = {1, 2, 0.5f, 1, 1, 0, 1, 0, 0.5f};
    auto with = [&](const std::string& find, const std::string& repl) {
        std::string j = a2_submodel(1, {1}, {1}, 0.1f, 1, w);
        j.replace(j.find(find), find.size(), repl);
        return j;
    };
    auto insert_before_head = [&](const std::string& frag) {
        std::string j = a2_submodel(1, {1}, {1}, 0.1f, 1, w);
        j.insert(j.find("\"head\":"), frag);
        return j;
    };

    CHECK_THAT(err_for(with("\"bottleneck\":1", "\"bottleneck\":2")),
               ContainsSubstring("bottleneck != channels"));
    CHECK_THAT(err_for(insert_before_head("\"gating_mode\":[\"gated\"],")),
               ContainsSubstring("gating"));
    CHECK_THAT(err_for(insert_before_head("\"conv_pre_film\":{\"active\":true},")),
               ContainsSubstring("FiLM"));
    CHECK_THAT(err_for(with("\"condition_size\":1", "\"condition_size\":2")),
               ContainsSubstring("condition_size != 1"));
    CHECK_THAT(err_for(with("\"out_channels\":1", "\"out_channels\":2")),
               ContainsSubstring("head out_channels != 1"));
    CHECK_THAT(err_for(with("\"channels\":1", "\"channels\":0")),
               ContainsSubstring("layer dimension"));   // nonpositive dim rejected
    CHECK_THAT(err_for(with("\"channels\":1", "\"channels\":1e100")),
               ContainsSubstring("layer dimension"));   // out-of-range int rejected
    CHECK_THAT(err_for(with("{\"type\":\"LeakyReLU\",\"negative_slope\":0.1}",
                            "{\"type\":\"LeakyyReLU\",\"negative_slope\":0.1}")),
               ContainsSubstring("activation"));         // typo'd activation rejected

    // Non-finite weight: JSON has no nan/inf literal, but an overflowing exponent
    // (1e400) parses to infinity — inject it as a raw token past the sentinel tail.
    {
        std::string j = a2_submodel(1, {1}, {1}, 0.1f, 1, {1, 2, 0.5f, 1, 1, 0, 1, 0, 0.25f});
        j.replace(j.rfind("0.25]"), 5, "1e400]");
        CHECK_THAT(err_for(j), ContainsSubstring("non-finite weight"));
    }

    // Weight-count mismatch (too few) surfaces from A2Model::build.
    CHECK_THAT(err_for(a2_submodel(1, {1}, {1}, 0.1f, 1, {1, 2, 0.5f})),
               ContainsSubstring("weight count mismatch"));

    // Multi-array WaveNet is rejected (A2 captures are single-array).
    const std::string one_layer =
        "{\"input_size\":1,\"condition_size\":1,\"channels\":1,\"bottleneck\":1,"
        "\"kernel_sizes\":[1],\"dilations\":[1],"
        "\"activation\":[{\"type\":\"LeakyReLU\",\"negative_slope\":0.1}],"
        "\"head\":{\"out_channels\":1,\"kernel_size\":1,\"bias\":true}}";
    const std::string two_array =
        "{\"architecture\":\"WaveNet\",\"config\":{\"layers\":[" + one_layer + "," + one_layer
        + "],\"head_scale\":1.0},\"weights\":[1,2,0.5,1,1,0,1,0,1,2,0.5,1,1,0,1,0,0.5]}";
    CHECK_THAT(err_for(two_array), ContainsSubstring("multi-array"));

    // Duplicate max_value in a container is ambiguous.
    const std::string sub = a2_submodel(1, {1}, {1}, 1.0f, 1,
                                        {1, 2, 0, 0, 1, 0, 1, 0, 1});
    CHECK_THAT(err_for(a2_container({{0.5, sub}, {0.5, sub}})),
               ContainsSubstring("duplicate submodel max_value"));
}

TEST_CASE("A2-shaped WaveNet is never mis-parsed by the A1 loader", "[nam][a2]") {
    // Even in a build with A2 compiled out, an A2-shaped "WaveNet" file must be
    // rejected by the A1 loader rather than read with missing scalar fields as zero.
    const std::string a2wave = a2_submodel(1, {1}, {1}, 0.1f, 2,
                                           {1, 1, 0, 0, 1, 0, 1, 1, 0, 1});
    const std::string path = write_temp("gpu_nam_a2_vs_a1.nam", a2wave);
    NamModel a1;
    std::string err;
    CHECK_FALSE(load_nam(path, a1, &err));
    CHECK_THAT(err, ContainsSubstring("A2-shaped"));
    std::filesystem::remove(path);

    // A2 markers in a LATER layer array must still be detected by the classifier.
    const std::string mixed =
        "{\"architecture\":\"WaveNet\",\"config\":{\"layers\":[{\"input_size\":1,"
        "\"condition_size\":1,\"channels\":1,\"kernel_size\":1,\"head_size\":1,"
        "\"activation\":\"Tanh\",\"dilations\":[1]},{\"input_size\":1,\"condition_size\":1,"
        "\"channels\":1,\"kernel_sizes\":[1],\"dilations\":[1]}],\"head_scale\":1.0},"
        "\"weights\":[]}";
    CHECK(is_nam_a2(choc::json::parse(mixed)));
}
