#pragma once

// Exact, reference-faithful CPU inference for the open-source Neural Amp Modeler
// (NAM) "LSTM" architecture, plus its slice of the ``.nam`` loader. LSTM captures
// are the second-most-common NAM format after WaveNet (the recurrent option in
// the training tool), so loading them is what makes the demo accept the bulk of
// real-world .nam files. WaveNet lives in nam_model.hpp; this file is the LSTM
// peer so neither header grows two architectures' worth of state.
//
// A NAM LSTM ``.nam`` file is JSON:
//   { architecture:"LSTM",
//     config:{ input_size, hidden_size, num_layers },
//     weights:[ flat f32 ... ], sample_rate }
//
// The flat ``weights`` array is a stack of standard LSTM cells followed by a
// linear head. Each cell is a textbook LSTM with the PyTorch gate order
// [input, forget, cell, output] packed as one matrix over the concatenated
// [x; h] vector:
//
//   for each layer l (input size i_l = input_size for l==0 else hidden_size):
//     W   [4*hidden][ i_l + hidden ]   row-major (rows are the 4 gate blocks of
//                                       ``hidden`` rows each, in i/f/g/o order;
//                                       the first i_l columns weight x, the rest
//                                       weight the recurrent h)
//     b   [4*hidden]
//     h0  [hidden]                      cell's initial hidden state (reset value)
//     c0  [hidden]                      cell's initial cell state (reset value)
//   head:
//     Wh  [out * hidden]                linear over the last layer's hidden state
//     bh  [out]
//
// (Weight-layout note: the initial (h0,c0) sit inside each cell's block — right
// after W,b — and the linear head is the final block. This order, and the h0/c0
// pairing, were pinned by matching a reference render of a real LSTM capture, not
// transcribed from any implementation.)
//
// The LSTM cell forward, per sample, with previous (h,c):
//   z = W [x; h] + b
//   i = sigmoid(z_i);  f = sigmoid(z_f);  g = tanh(z_g);  o = sigmoid(z_o)
//   c' = f*c + i*g;    h' = o*tanh(c')
// The model output is the linear head over the last layer's h'. NAM's LSTM has no
// separate input/output gain here — the head scale is folded into the head weights.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::nam {

// One LSTM cell: standard forward with the NAM weight layout. Stateful — carries
// (h,c) across samples, so step() can be called sample-by-sample like a real-time
// engine. reset() restores the file's stored initial (h0,c0).
class LstmCell {
public:
    void configure(int input_size, int hidden_size) {
        in_ = input_size;
        h_ = hidden_size;
        const std::size_t rows = static_cast<std::size_t>(4 * h_);
        const std::size_t cols = static_cast<std::size_t>(in_ + h_);
        w_.assign(rows * cols, 0.0f);
        b_.assign(rows, 0.0f);
        h0_.assign(static_cast<std::size_t>(h_), 0.0f);
        c0_.assign(static_cast<std::size_t>(h_), 0.0f);
        h_state_.assign(static_cast<std::size_t>(h_), 0.0f);
        c_state_.assign(static_cast<std::size_t>(h_), 0.0f);
        xh_.assign(cols, 0.0f);
        z_.assign(rows, 0.0f);
    }

    std::size_t weight_count() const {
        const std::size_t rows = static_cast<std::size_t>(4 * h_);
        const std::size_t cols = static_cast<std::size_t>(in_ + h_);
        return rows * cols + rows + 2u * static_cast<std::size_t>(h_);  // W + b + h0 + c0
    }

    void load(const float*& p) {
        const std::size_t rows = static_cast<std::size_t>(4 * h_);
        const std::size_t cols = static_cast<std::size_t>(in_ + h_);
        for (std::size_t r = 0; r < rows; ++r)
            for (std::size_t c = 0; c < cols; ++c) w_[r * cols + c] = *p++;
        for (std::size_t r = 0; r < rows; ++r) b_[r] = *p++;
        for (int j = 0; j < h_; ++j) h0_[static_cast<std::size_t>(j)] = *p++;
        for (int j = 0; j < h_; ++j) c0_[static_cast<std::size_t>(j)] = *p++;
        reset();
    }

    void reset() {
        h_state_ = h0_;
        c_state_ = c0_;
    }

    // x: in_ values -> writes h_ output values (the new hidden state) to out.
    void step(const float* x, float* out) {
        const std::size_t cols = static_cast<std::size_t>(in_ + h_);
        for (int j = 0; j < in_; ++j) xh_[static_cast<std::size_t>(j)] = x[j];
        for (int j = 0; j < h_; ++j) xh_[static_cast<std::size_t>(in_ + j)] = h_state_[static_cast<std::size_t>(j)];

        const std::size_t rows = static_cast<std::size_t>(4 * h_);
        for (std::size_t r = 0; r < rows; ++r) {
            const float* wr = &w_[r * cols];
            float acc = b_[r];
            for (std::size_t c = 0; c < cols; ++c) acc += wr[c] * xh_[c];
            z_[r] = acc;
        }
        // Gate blocks: i, f, g, o — each ``h_`` wide.
        const int H = h_;
        for (int j = 0; j < H; ++j) {
            const float ig = sigmoid(z_[static_cast<std::size_t>(0 * H + j)]);
            const float fg = sigmoid(z_[static_cast<std::size_t>(1 * H + j)]);
            const float gg = std::tanh(z_[static_cast<std::size_t>(2 * H + j)]);
            const float og = sigmoid(z_[static_cast<std::size_t>(3 * H + j)]);
            const float c_new = fg * c_state_[static_cast<std::size_t>(j)] + ig * gg;
            c_state_[static_cast<std::size_t>(j)] = c_new;
            const float h_new = og * std::tanh(c_new);
            h_state_[static_cast<std::size_t>(j)] = h_new;
            out[j] = h_new;
        }
    }

    int input_size() const { return in_; }
    int hidden_size() const { return h_; }

private:
    static float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

    int in_ = 1, h_ = 1;
    std::vector<float> w_;        // [4h x (in+h)] row-major
    std::vector<float> b_;        // [4h]
    std::vector<float> h0_, c0_;  // stored reset state
    std::vector<float> h_state_, c_state_;
    std::vector<float> xh_, z_;   // scratch
};

// The full NAM LSTM model: a stack of cells + a linear head over the last hidden
// state. Stateful streaming inference (process_sample carries state across calls).
class NamLstmModel {
public:
    // Build from parsed config + flat weights. Returns false (sets error()) on a
    // weight-count mismatch or a degenerate shape.
    bool build(int input_size, int hidden_size, int num_layers, int out_size,
               std::vector<float> weights, double sample_rate) {
        input_size_ = input_size;
        hidden_size_ = hidden_size;
        num_layers_ = num_layers;
        out_size_ = out_size > 0 ? out_size : 1;
        weights_ = std::move(weights);
        sample_rate_ = sample_rate;
        error_.clear();

        if (hidden_size_ <= 0 || num_layers_ <= 0) {
            error_ = "LSTM requires positive hidden_size and num_layers";
            return false;
        }
        // The runtime feeds a 1-wide (mono) input sample; a model whose first layer
        // expects more than one input channel would read past that buffer. NAM
        // captures are mono, so require it rather than mis-render.
        if (input_size_ != 1) {
            error_ = "LSTM input_size must be 1 (mono); got " + std::to_string(input_size_);
            return false;
        }

        cells_.clear();
        cells_.resize(static_cast<std::size_t>(num_layers_));
        for (int l = 0; l < num_layers_; ++l)
            cells_[static_cast<std::size_t>(l)].configure(l == 0 ? input_size_ : hidden_size_, hidden_size_);

        std::size_t expected = 0;
        for (const LstmCell& c : cells_) expected += c.weight_count();
        expected += static_cast<std::size_t>(out_size_) * static_cast<std::size_t>(hidden_size_)  // head W
                    + static_cast<std::size_t>(out_size_);                                          // head b
        expected_weight_count_ = expected;

        if (weights_.size() != expected) {
            error_ = "LSTM weight count mismatch: file provides " + std::to_string(weights_.size())
                     + " floats, model layout consumes " + std::to_string(expected);
            return false;
        }

        const float* p = weights_.data();
        for (LstmCell& c : cells_) c.load(p);
        const std::size_t hw = static_cast<std::size_t>(out_size_) * static_cast<std::size_t>(hidden_size_);
        head_w_.assign(hw, 0.0f);
        head_b_.assign(static_cast<std::size_t>(out_size_), 0.0f);
        for (std::size_t i = 0; i < hw; ++i) head_w_[i] = *p++;
        for (int i = 0; i < out_size_; ++i) head_b_[static_cast<std::size_t>(i)] = *p++;
        weights_consumed_ = static_cast<std::size_t>(p - weights_.data());

        hbuf_a_.assign(static_cast<std::size_t>(hidden_size_), 0.0f);
        hbuf_b_.assign(static_cast<std::size_t>(hidden_size_), 0.0f);
        return weights_consumed_ == weights_.size();
    }

    void reset() {
        for (LstmCell& c : cells_) c.reset();
    }

    // Settle at the silence steady-state (reset to the stored initial state, then
    // run silence). For a NAM LSTM the stored (h0,c0) is already the resting state,
    // so this mostly confirms it; kept for parity with the WaveNet prewarm and to
    // absorb any residual transient. Off-thread only.
    void prewarm() {
        reset();
        for (int i = 0; i < 1024; ++i) process_sample(0.0f);
    }

    float process_sample(float x) {
        float in0[1] = {x};
        const float* layer_in = in0;
        std::vector<float>* cur = &hbuf_a_;
        std::vector<float>* prev = &hbuf_b_;
        for (std::size_t l = 0; l < cells_.size(); ++l) {
            cells_[l].step(layer_in, cur->data());
            layer_in = cur->data();
            std::swap(cur, prev);
        }
        // Last layer's hidden state lives in ``prev`` after the final swap.
        const std::vector<float>& hlast = *prev;
        float y = head_b_.empty() ? 0.0f : head_b_[0];
        for (int j = 0; j < hidden_size_; ++j) y += head_w_[static_cast<std::size_t>(j)] * hlast[static_cast<std::size_t>(j)];
        return y;
    }

    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) out[i] = process_sample(in[i]);
    }

    int input_size() const { return input_size_; }
    int hidden_size() const { return hidden_size_; }
    int num_layers() const { return num_layers_; }
    int out_channels() const { return out_size_; }
    double sample_rate() const { return sample_rate_; }
    std::size_t weights_size() const { return weights_.size(); }
    std::size_t weights_consumed() const { return weights_consumed_; }
    std::size_t expected_weight_count() const { return expected_weight_count_; }
    const std::string& error() const { return error_; }

private:
    int input_size_ = 1, hidden_size_ = 1, num_layers_ = 1, out_size_ = 1;
    std::vector<LstmCell> cells_;
    std::vector<float> head_w_, head_b_;
    std::vector<float> weights_;
    double sample_rate_ = -1.0;
    std::size_t weights_consumed_ = 0, expected_weight_count_ = 0;
    std::string error_;
    std::vector<float> hbuf_a_, hbuf_b_;
};

// Parse a NAM "LSTM" .nam file into ``out``. Returns false + sets ``error`` on any
// failure. Shares choc::json with the WaveNet loader; kept here so the LSTM
// architecture is fully self-contained. ``in_channels``/``out_channels`` are the
// audio channel counts (mono captures: 1/1); the head maps the last hidden state
// to ``out_channels`` outputs.
inline bool load_nam_lstm(const std::string& path, NamLstmModel& out, std::string* error = nullptr) {
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
    if (!root.isObject()) return fail("top-level JSON is not an object");
    if (!root.hasObjectMember("config")) return fail("missing 'config'");
    const choc::value::ValueView config = root["config"];

    auto num = [](const choc::value::ValueView& v) -> double {
        if (v.isInt64()) return static_cast<double>(v.getInt64());
        if (v.isInt32()) return static_cast<double>(v.getInt32());
        if (v.isFloat64()) return v.getFloat64();
        if (v.isFloat32()) return static_cast<double>(v.getFloat32());
        return v.getWithDefault<double>(0.0);
    };

    const int input_size = config.hasObjectMember("input_size")
                               ? static_cast<int>(num(config["input_size"])) : 1;
    const int hidden_size = config.hasObjectMember("hidden_size")
                                ? static_cast<int>(num(config["hidden_size"])) : 0;
    const int num_layers = config.hasObjectMember("num_layers")
                               ? static_cast<int>(num(config["num_layers"])) : 0;
    // NAM LSTM captures are mono in/out; the head maps to one audio channel.
    const int out_channels = 1;
    if (hidden_size <= 0 || num_layers <= 0)
        return fail("LSTM config needs positive hidden_size and num_layers");

    const double sample_rate =
        root.hasObjectMember("sample_rate") ? num(root["sample_rate"]) : -1.0;

    if (!root.hasObjectMember("weights")) return fail("missing 'weights'");
    const choc::value::ValueView wv = root["weights"];
    if (!wv.isArray()) return fail("'weights' must be an array");
    std::vector<float> weights;
    weights.reserve(wv.size());
    for (uint32_t i = 0; i < wv.size(); ++i) weights.push_back(static_cast<float>(num(wv[i])));

    const bool ok = out.build(input_size, hidden_size, num_layers, out_channels,
                              std::move(weights), sample_rate);
    if (!ok && error) *error = out.error();
    return ok;
}

}  // namespace pulp::examples::nam
