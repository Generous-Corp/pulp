#pragma once

// NAM "Linear" architecture — the simplest NAM model: a single causal FIR
// (an impulse response) with an optional bias. y[n] = bias + sum_k ir[k]*x[n-k].
//
// The .nam JSON is { architecture:"Linear", config:{ receptive_field, bias,
// in_channels?, out_channels? }, weights:[ ir[0..RF-1], (bias) ], sample_rate }.
// The flat weights are the impulse response in forward order, followed by the
// single bias term when config.bias is true. Amp captures are mono (1-in/1-out);
// a multi-channel Linear is rejected rather than mis-rendered.
//
// This is clean-room: written from the model math and the .nam layout, not ported
// from any implementation. The reference engine convolves via FFT/partitions for
// speed, but the result is exactly this direct FIR — so a direct implementation is
// the faithful oracle, and small captures are the only ones seen in practice.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::nam {

// Stateful streaming FIR. process_sample carries the input history across calls.
class NamLinearModel {
public:
    // Build from parsed config + flat weights. Returns false (sets error()) on a
    // channel/size mismatch, leaving any previously-built state untouched. Mono
    // only (in==out==1). Validates fully before mutating, and clears error() on
    // success so ok() never reflects a stale prior failure.
    bool build(int receptive_field, bool bias, int in_channels, int out_channels,
               std::vector<float> weights, double sample_rate) {
        if (in_channels != 1 || out_channels != 1) {
            error_ = "Linear: only 1-in/1-out (mono) is supported";
            return false;
        }
        if (receptive_field <= 0) {
            error_ = "Linear: receptive_field must be positive";
            return false;
        }
        const std::size_t expected =
            static_cast<std::size_t>(receptive_field) + (bias ? 1u : 0u);
        if (weights.size() != expected) {
            error_ = "Linear weight count mismatch: file provides "
                     + std::to_string(weights.size()) + ", expected "
                     + std::to_string(expected);
            return false;
        }
        // All checks passed — commit.
        rf_ = receptive_field;
        ir_.assign(weights.begin(), weights.begin() + receptive_field);
        bias_ = bias ? weights[static_cast<std::size_t>(receptive_field)] : 0.0f;
        sample_rate_ = sample_rate;
        hist_.assign(static_cast<std::size_t>(rf_), 0.0f);
        pos_ = 0;
        error_.clear();
        return true;
    }

    void reset() {
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        pos_ = 0;
    }

    // A pure FIR has no multi-sample settling transient: with a zeroed history,
    // the silence steady-state (output == bias) is reached immediately. So prewarm
    // is exactly reset — running silence would only spin O(rf^2) work to the same
    // state. Kept for interface parity so callers can prewarm any architecture.
    void prewarm() { reset(); }

    float process_sample(float x) {
        if (rf_ <= 0) return x;
        hist_[static_cast<std::size_t>(pos_)] = x;
        float acc = bias_;
        int idx = pos_;                              // k=0 reads x[n]
        for (int k = 0; k < rf_; ++k) {
            acc += ir_[static_cast<std::size_t>(k)] * hist_[static_cast<std::size_t>(idx)];
            idx = (idx == 0) ? rf_ - 1 : idx - 1;    // step one sample into the past
        }
        pos_ = (pos_ + 1 == rf_) ? 0 : pos_ + 1;
        return acc;
    }

    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) out[i] = process_sample(in[i]);
    }

    bool ok() const { return rf_ > 0; }
    double sample_rate() const { return sample_rate_; }
    int receptive_field() const { return rf_; }
    const std::string& error() const { return error_; }

private:
    int rf_ = 0;
    float bias_ = 0.0f;
    std::vector<float> ir_;
    std::vector<float> hist_;
    int pos_ = 0;
    double sample_rate_ = -1.0;
    std::string error_;
};

// Load a Linear .nam. Returns false + sets *error on failure.
inline bool load_nam_linear(const std::string& path, NamLinearModel& out,
                            std::string* error = nullptr) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("could not open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return fail("empty file: " + path);

    choc::value::Value root;
    try {
        root = choc::json::parse(text);
    } catch (const std::exception& e) {
        return fail(std::string("JSON parse error: ") + e.what());
    }
    if (root.isObject() && root.hasObjectMember("architecture")
        && std::string(root["architecture"].getString()) != "Linear")
        return fail("not a Linear model (architecture='"
                    + std::string(root["architecture"].getString()) + "')");
    if (!root.hasObjectMember("config")) return fail("missing 'config'");
    const choc::value::ValueView config = root["config"];

    auto num = [](const choc::value::ValueView& v) -> double {
        if (v.isInt64()) return static_cast<double>(v.getInt64());
        if (v.isFloat64()) return v.getFloat64();
        return v.getWithDefault<double>(0.0);
    };

    if (!config.hasObjectMember("receptive_field"))
        return fail("Linear: missing config.receptive_field");
    const int receptive_field = static_cast<int>(num(config["receptive_field"]));
    const bool bias = config.hasObjectMember("bias")
                          && config["bias"].getWithDefault<bool>(false);
    const int in_channels = config.hasObjectMember("in_channels")
                                ? static_cast<int>(num(config["in_channels"])) : 1;
    const int out_channels = config.hasObjectMember("out_channels")
                                 ? static_cast<int>(num(config["out_channels"])) : 1;
    const double sample_rate =
        root.hasObjectMember("sample_rate") ? num(root["sample_rate"]) : -1.0;

    if (!root.hasObjectMember("weights")) return fail("missing 'weights'");
    const choc::value::ValueView wv = root["weights"];
    if (!wv.isArray()) return fail("'weights' must be an array");
    std::vector<float> weights;
    weights.reserve(wv.size());
    for (uint32_t i = 0; i < wv.size(); ++i)
        weights.push_back(static_cast<float>(num(wv[i])));

    if (!out.build(receptive_field, bias, in_channels, out_channels,
                   std::move(weights), sample_rate))
        return fail(out.error());
    return true;
}

}  // namespace pulp::examples::nam
