// Tests for the RTNeural/Keras (.json) CPU inference engine: the Dense affine,
// the standalone activation layer, and the recurrent GRU + LSTM cells, plus the
// JSON loader and its rejections.
//
// The Dense goldens are exact hand arithmetic. The GRU and LSTM goldens are
// computed independently from the published reset-after GRU / Keras-LSTM cell
// equations on single-unit models (see the derivation in each test), so they pin
// the gate order, the two-bias GRU convention, and the i,f,c,o LSTM order — not
// just self-consistency.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "keras_runtime.hpp"

using namespace pulp::examples::keras;
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

KerasModel load_or_fail(const std::string& name, const std::string& json) {
    const std::string path = write_temp(name, json);
    KerasModel m;
    std::string err;
    const bool ok = load_keras_model(path, m, &err);
    std::filesystem::remove(path);
    INFO("load error: " << err);
    REQUIRE(ok);
    return m;
}

}  // namespace

TEST_CASE("Keras Dense chain is exact", "[keras][dense]") {
    // Layer 0: dense 1->2, kernel[in][out]=[[2,3]], bias[1,-1] -> [2x+1, 3x-1].
    // Layer 1: dense 2->1, kernel[[1],[1]], bias[0] -> (2x+1)+(3x-1) = 5x.
    const std::string json =
        R"({"in_shape":[null,1],"layers":[)"
        R"({"type":"dense","activation":"","shape":[null,2],)"
        R"("weights":[[[2.0,3.0]],[1.0,-1.0]]},)"
        R"({"type":"dense","activation":"","shape":[null,1],)"
        R"("weights":[[[1.0],[1.0]],[0.0]]}]})";
    KerasModel m = load_or_fail("keras_dense_chain.json", json);
    CHECK(m.in_size() == 1);
    CHECK(m.out_size() == 1);
    CHECK_THAT(m.process_sample(2.0f), WithinAbs(10.0f, 1e-6));   // 5*2
    CHECK_THAT(m.process_sample(-3.0f), WithinAbs(-15.0f, 1e-6)); // 5*-3
}

TEST_CASE("Keras Dense applies its activation", "[keras][dense]") {
    // dense 1->1, kernel[[2]], bias[1], ReLU -> relu(2x+1).
    const std::string json =
        R"({"in_shape":[null,1],"layers":[)"
        R"({"type":"dense","activation":"relu","shape":[null,1],)"
        R"("weights":[[[2.0]],[1.0]]}]})";
    KerasModel m = load_or_fail("keras_dense_relu.json", json);
    CHECK_THAT(m.process_sample(1.0f), WithinAbs(3.0f, 1e-6));   // relu(3)
    CHECK_THAT(m.process_sample(-1.0f), WithinAbs(0.0f, 1e-6));  // relu(-1)
}

TEST_CASE("Keras standalone activation layer", "[keras][activation]") {
    // dense 1->1 identity then a standalone tanh activation.
    const std::string json =
        R"({"in_shape":[null,1],"layers":[)"
        R"({"type":"dense","activation":"","shape":[null,1],"weights":[[[1.0]],[0.0]]},)"
        R"({"type":"activation","activation":"tanh","shape":[null,1],"weights":[]}]})";
    KerasModel m = load_or_fail("keras_act.json", json);
    CHECK_THAT(m.process_sample(0.5f), WithinAbs(std::tanh(0.5f), 1e-6));
    CHECK_THAT(m.process_sample(-2.0f), WithinAbs(std::tanh(-2.0f), 1e-6));
}

TEST_CASE("Keras GRU matches the reset-after cell", "[keras][gru]") {
    // Single unit. kernel[1][3]=[Wz,Wr,Wc], recurrent[1][3]=[Uz,Ur,Uc],
    // bias[2][3] = [[bz0,br0,bc0],[bz1,br1,bc1]]. Reset-after:
    //   z=sig(Wz*x+Uz*h+bz0+bz1); r=sig(Wr*x+Ur*h+br0+br1)
    //   c=tanh(Wc*x + r*(Uc*h+bc1) + bc0); h=(1-z)*c+z*h.
    // Goldens computed independently from that formula (see test derivation).
    const std::string json =
        R"({"in_shape":[null,null,1],"layers":[)"
        R"({"type":"gru","activation":"tanh","shape":[null,null,1],"weights":[)"
        R"([[0.5,-0.3,0.8]],)"            // kernel[in=1][3]
        R"([[0.1,0.2,-0.4]],)"            // recurrent[out=1][3]
        R"([[0.0,0.1,-0.1],[0.2,0.0,0.05]]])"  // bias[2][3]
        R"(}]})";
    KerasModel m = load_or_fail("keras_gru.json", json);
    const std::vector<float> in = {1.0f, 0.5f, -1.0f};
    const std::vector<float> golden = {0.2052127888f, 0.2326722611f, -0.3141872308f};
    for (std::size_t i = 0; i < in.size(); ++i)
        CHECK_THAT(m.process_sample(in[i]), WithinAbs(golden[i], 1e-6));
}

TEST_CASE("Keras LSTM matches the i,f,c,o cell", "[keras][lstm]") {
    // Single unit. kernel[1][4]=[Wi,Wf,Wc,Wo], recurrent[1][4]=[Ui,Uf,Uc,Uo],
    // bias[4]=[bi,bf,bc,bo]:
    //   i=sig(Wi*x+Ui*h+bi); f=sig(Wf*x+Uf*h+bf); o=sig(Wo*x+Uo*h+bo)
    //   g=tanh(Wc*x+Uc*h+bc); c=f*c+i*g; h=o*tanh(c).
    const std::string json =
        R"({"in_shape":[null,null,1],"layers":[)"
        R"({"type":"lstm","activation":"tanh","shape":[null,null,1],"weights":[)"
        R"([[0.6,-0.2,0.4,0.3]],)"        // kernel[in=1][4]
        R"([[0.1,0.05,-0.1,0.2]],)"       // recurrent[out=1][4]
        R"([0.0,0.1,-0.05,0.2])"          // bias[4]
        R"(]}]})";
    KerasModel m = load_or_fail("keras_lstm.json", json);
    const std::vector<float> in = {1.0f, 0.5f, -1.0f};
    const std::vector<float> golden = {0.1331014019f, 0.1098728869f, -0.0220149096f};
    for (std::size_t i = 0; i < in.size(); ++i)
        CHECK_THAT(m.process_sample(in[i]), WithinAbs(golden[i], 1e-6));
}

TEST_CASE("Keras GRU->Dense state resets", "[keras][gru]") {
    const std::string json =
        R"({"in_shape":[null,null,1],"layers":[)"
        R"({"type":"gru","activation":"tanh","shape":[null,null,1],"weights":[)"
        R"([[0.5,-0.3,0.8]],[[0.1,0.2,-0.4]],[[0.0,0.1,-0.1],[0.2,0.0,0.05]]]}]})";
    KerasModel m = load_or_fail("keras_gru_reset.json", json);
    const float a = m.process_sample(1.0f);
    m.process_sample(1.0f);
    m.reset();
    const float b = m.process_sample(1.0f);
    CHECK_THAT(a, WithinAbs(b, 1e-7));   // same first-sample output after reset
}

TEST_CASE("Keras loader rejects malformed models", "[keras][reject]") {
    auto rejects = [](const std::string& name, const std::string& json,
                      const std::string& needle) {
        const std::string path = write_temp(name, json);
        KerasModel m;
        std::string err;
        const bool ok = load_keras_model(path, m, &err);
        std::filesystem::remove(path);
        CHECK_FALSE(ok);
        CHECK_THAT(err, ContainsSubstring(needle));
    };

    // Unknown layer type.
    rejects("keras_bad_type.json",
            R"({"in_shape":[null,1],"layers":[{"type":"conv1d","shape":[null,1],"weights":[]}]})",
            "unsupported layer type");
    // Unsupported activation (elu) on a dense layer.
    rejects("keras_bad_act.json",
            R"({"in_shape":[null,1],"layers":[{"type":"dense","activation":"elu","shape":[null,1],"weights":[[[1.0]],[0.0]]}]})",
            "unsupported activation");
    // Standalone activation that changes width.
    rejects("keras_act_width.json",
            R"({"in_shape":[null,1],"layers":[{"type":"activation","activation":"tanh","shape":[null,4],"weights":[]}]})",
            "changes width");
    // GRU with too few weight sets.
    rejects("keras_gru_short.json",
            R"({"in_shape":[null,null,1],"layers":[{"type":"gru","shape":[null,null,1],"weights":[[[0.1,0.2,0.3]]]}]})",
            "needs 3 weight sets");
    // Missing layers array.
    rejects("keras_no_layers.json", R"({"in_shape":[null,1]})", "missing 'layers'");
    // Invalid input shape.
    rejects("keras_bad_in.json",
            R"({"in_shape":[null,0],"layers":[{"type":"dense","shape":[null,1],"weights":[[[1.0]],[0.0]]}]})",
            "invalid 'in_shape'");
}

TEST_CASE("Keras loader rejects hostile weight shapes", "[keras][reject]") {
    auto rejects = [](const std::string& name, const std::string& json,
                      const std::string& needle) {
        const std::string path = write_temp(name, json);
        KerasModel m;
        std::string err;
        const bool ok = load_keras_model(path, m, &err);
        std::filesystem::remove(path);
        CHECK_FALSE(ok);
        CHECK_THAT(err, ContainsSubstring(needle));
        // The failed load must leave an inert model: process_sample is a safe
        // pass-through, never an out-of-bounds read into a half-built chain.
        CHECK_THAT(m.process_sample(0.5f), WithinAbs(0.5f, 1e-6));
    };

    // GRU with a ragged kernel row (1 col where 3*out=3 are needed).
    rejects("keras_gru_ragged.json",
            R"({"in_shape":[null,null,1],"layers":[{"type":"gru","shape":[null,null,1],)"
            R"("weights":[[[0.1]],[[0.2]],[[0.0],[0.0]]]}]})",
            "weight shape mismatch");
    // LSTM with a short bias vector (1 where 4*out=4 are needed).
    rejects("keras_lstm_ragged.json",
            R"({"in_shape":[null,null,1],"layers":[{"type":"lstm","shape":[null,null,1],)"
            R"("weights":[[[0.1]],[[0.2]],[0.0]]}]})",
            "weight shape mismatch");
    // Dense with a bias too short for its output width.
    rejects("keras_dense_bias.json",
            R"({"in_shape":[null,1],"layers":[{"type":"dense","shape":[null,2],)"
            R"("weights":[[[1.0,1.0]],[1.0]]}]})",
            "bias too short");
    // Multi-input model (the scalar API only supports one input channel).
    rejects("keras_multi_in.json",
            R"({"in_shape":[null,2],"layers":[{"type":"dense","shape":[null,1],)"
            R"("weights":[[[1.0],[1.0]],[0.0]]}]})",
            "single-input");
    // A wide final layer would be silently truncated to one channel.
    rejects("keras_wide_tail.json",
            R"({"in_shape":[null,1],"layers":[{"type":"dense","shape":[null,2],)"
            R"("weights":[[[1.0,1.0]],[0.0,0.0]]}]})",
            "single channel");
    // A recurrent layer with a non-tanh activation (the cell hard-codes tanh).
    rejects("keras_gru_relu.json",
            R"({"in_shape":[null,null,1],"layers":[{"type":"gru","activation":"relu","shape":[null,null,1],)"
            R"("weights":[[[0.5,-0.3,0.8]],[[0.1,0.2,-0.4]],[[0.0,0.1,-0.1],[0.2,0.0,0.05]]]}]})",
            "unsupported activation");
    // An absurd layer width is rejected before it drives a huge allocation.
    rejects("keras_huge.json",
            R"({"in_shape":[null,1],"layers":[{"type":"dense","shape":[null,99999999],"weights":[[[1.0]],[0.0]]}]})",
            "width too large");
}
