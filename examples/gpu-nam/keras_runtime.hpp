#pragma once

// RTNeural-style Keras model CPU inference — a separate runtime from NamRuntime,
// for the ``.json`` models RTNeural exports (a common format for neural guitar-amp
// captures). Supports a sequential stack of GRU and Dense layers with per-layer
// activations, which covers the usual recurrent amp topology (GRU -> Dense).
//
// The GRU is the Keras "reset-after" variant (two bias vectors per gate; the reset
// gate multiplies the recurrent term including its bias), matching RTNeural's
// reference cell exactly:
//   z = sigmoid(Wz.x + Uz.h + bz0 + bz1)
//   r = sigmoid(Wr.x + Ur.h + br0 + br1)
//   c = tanh(Wc.x + r*(Uc.h + bc1) + bc0)
//   h' = (1-z)*c + z*h
//
// Recurrent layers are sequential over time at small hidden sizes, so this is a
// CPU-only engine (no GPU path) — the same reasoning as the NAM LSTM.
//
// JSON layout (RTNeural export): { in_shape:[..,in], layers:[ {type, activation,
// shape:[..,out], weights} ] }. GRU weights = [kernel[in][3*out],
// recurrent[out][3*out], bias[2][3*out]] with gate order [z, r, c]. Dense weights =
// [kernel[in][out], bias[out]].
//
// Depends only on choc (JSON) + the standard library.

#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::keras {

enum class Activation { None, Tanh, Sigmoid, ReLU };

inline Activation parse_activation(const std::string& name) {
    if (name.empty() || name == "linear") return Activation::None;
    if (name == "tanh") return Activation::Tanh;
    if (name == "sigmoid") return Activation::Sigmoid;
    if (name == "relu") return Activation::ReLU;
    return Activation::None;  // unknown -> identity (caller may reject upstream)
}

inline float apply_activation(Activation a, float x) {
    switch (a) {
        case Activation::Tanh:    return std::tanh(x);
        case Activation::Sigmoid: return 1.0f / (1.0f + std::exp(-x));
        case Activation::ReLU:    return x > 0.0f ? x : 0.0f;
        default:                  return x;
    }
}

inline float dot(const float* a, const float* b, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

// Base for a sequential layer. forward() maps in_size inputs to out_size outputs.
class KerasLayer {
public:
    virtual ~KerasLayer() = default;
    virtual void reset() {}
    virtual int in_size() const = 0;
    virtual int out_size() const = 0;
    virtual void forward(const float* in, float* out) = 0;
};

// Keras GRU (reset-after variant).
class KerasGru final : public KerasLayer {
public:
    KerasGru(int in, int out) : in_(in), out_(out) {
        const std::size_t io = static_cast<std::size_t>(in) * out;
        const std::size_t oo = static_cast<std::size_t>(out) * out;
        zW_.assign(io, 0); rW_.assign(io, 0); cW_.assign(io, 0);
        zU_.assign(oo, 0); rU_.assign(oo, 0); cU_.assign(oo, 0);
        zb0_.assign(out, 0); zb1_.assign(out, 0);
        rb0_.assign(out, 0); rb1_.assign(out, 0);
        cb0_.assign(out, 0); cb1_.assign(out, 0);
        h_.assign(out, 0); uh_.assign(out, 0);
    }
    int in_size() const override { return in_; }
    int out_size() const override { return out_; }
    void reset() override { std::fill(h_.begin(), h_.end(), 0.0f); }

    // kernel[in][3*out], recurrent[out][3*out], bias[2][3*out]; gate order z,r,c.
    void load(const std::vector<std::vector<float>>& kernel,
              const std::vector<std::vector<float>>& recurrent,
              const std::vector<std::vector<float>>& bias) {
        for (int i = 0; i < out_; ++i)
            for (int k = 0; k < in_; ++k) {
                const auto idx = static_cast<std::size_t>(i) * in_ + k;
                zW_[idx] = kernel[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)];
                rW_[idx] = kernel[static_cast<std::size_t>(k)][static_cast<std::size_t>(i + out_)];
                cW_[idx] = kernel[static_cast<std::size_t>(k)][static_cast<std::size_t>(i + 2 * out_)];
            }
        for (int i = 0; i < out_; ++i)
            for (int m = 0; m < out_; ++m) {
                const auto idx = static_cast<std::size_t>(i) * out_ + m;
                zU_[idx] = recurrent[static_cast<std::size_t>(m)][static_cast<std::size_t>(i)];
                rU_[idx] = recurrent[static_cast<std::size_t>(m)][static_cast<std::size_t>(i + out_)];
                cU_[idx] = recurrent[static_cast<std::size_t>(m)][static_cast<std::size_t>(i + 2 * out_)];
            }
        for (int i = 0; i < out_; ++i) {
            zb0_[static_cast<std::size_t>(i)] = bias[0][static_cast<std::size_t>(i)];
            rb0_[static_cast<std::size_t>(i)] = bias[0][static_cast<std::size_t>(i + out_)];
            cb0_[static_cast<std::size_t>(i)] = bias[0][static_cast<std::size_t>(i + 2 * out_)];
            zb1_[static_cast<std::size_t>(i)] = bias[1][static_cast<std::size_t>(i)];
            rb1_[static_cast<std::size_t>(i)] = bias[1][static_cast<std::size_t>(i + out_)];
            cb1_[static_cast<std::size_t>(i)] = bias[1][static_cast<std::size_t>(i + 2 * out_)];
        }
    }

    void forward(const float* x, float* out) override {
        const float* h = h_.data();
        for (int i = 0; i < out_; ++i) {
            const std::size_t wi = static_cast<std::size_t>(i) * in_;
            const std::size_t ui = static_cast<std::size_t>(i) * out_;
            const float z = sigmoid(dot(&zW_[wi], x, in_) + dot(&zU_[ui], h, out_)
                                    + zb0_[static_cast<std::size_t>(i)] + zb1_[static_cast<std::size_t>(i)]);
            const float r = sigmoid(dot(&rW_[wi], x, in_) + dot(&rU_[ui], h, out_)
                                    + rb0_[static_cast<std::size_t>(i)] + rb1_[static_cast<std::size_t>(i)]);
            const float c = std::tanh(dot(&cW_[wi], x, in_)
                                      + r * (dot(&cU_[ui], h, out_) + cb1_[static_cast<std::size_t>(i)])
                                      + cb0_[static_cast<std::size_t>(i)]);
            uh_[static_cast<std::size_t>(i)] =
                (1.0f - z) * c + z * h_[static_cast<std::size_t>(i)];
        }
        std::copy(uh_.begin(), uh_.end(), h_.begin());
        std::copy(uh_.begin(), uh_.end(), out);
    }

private:
    static float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
    int in_, out_;
    std::vector<float> zW_, rW_, cW_, zU_, rU_, cU_;
    std::vector<float> zb0_, zb1_, rb0_, rb1_, cb0_, cb1_;
    std::vector<float> h_, uh_;
};

// Keras LSTM. Single bias vector; gate order i, f, c, o.
//   i = sigmoid(Wi.x + Ui.h + bi)
//   f = sigmoid(Wf.x + Uf.h + bf)
//   o = sigmoid(Wo.x + Uo.h + bo)
//   g = tanh(Wc.x + Uc.h + bc)
//   c = f*c_prev + i*g ;  h = o*tanh(c)
class KerasLstm final : public KerasLayer {
public:
    KerasLstm(int in, int out) : in_(in), out_(out) {
        const std::size_t io = static_cast<std::size_t>(in) * out;
        const std::size_t oo = static_cast<std::size_t>(out) * out;
        iW_.assign(io, 0); fW_.assign(io, 0); cW_.assign(io, 0); oW_.assign(io, 0);
        iU_.assign(oo, 0); fU_.assign(oo, 0); cU_.assign(oo, 0); oU_.assign(oo, 0);
        ib_.assign(out, 0); fb_.assign(out, 0); cb_.assign(out, 0); ob_.assign(out, 0);
        h_.assign(out, 0); c_.assign(out, 0); nh_.assign(out, 0); nc_.assign(out, 0);
    }
    int in_size() const override { return in_; }
    int out_size() const override { return out_; }
    void reset() override {
        std::fill(h_.begin(), h_.end(), 0.0f);
        std::fill(c_.begin(), c_.end(), 0.0f);
    }

    // kernel[in][4*out], recurrent[out][4*out], bias[4*out]; gate order i,f,c,o.
    void load(const std::vector<std::vector<float>>& kernel,
              const std::vector<std::vector<float>>& recurrent,
              const std::vector<float>& bias) {
        for (int k = 0; k < out_; ++k)
            for (int i = 0; i < in_; ++i) {
                const auto idx = static_cast<std::size_t>(k) * in_ + i;
                iW_[idx] = kernel[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)];
                fW_[idx] = kernel[static_cast<std::size_t>(i)][static_cast<std::size_t>(k + out_)];
                cW_[idx] = kernel[static_cast<std::size_t>(i)][static_cast<std::size_t>(k + 2 * out_)];
                oW_[idx] = kernel[static_cast<std::size_t>(i)][static_cast<std::size_t>(k + 3 * out_)];
            }
        for (int k = 0; k < out_; ++k)
            for (int m = 0; m < out_; ++m) {
                const auto idx = static_cast<std::size_t>(k) * out_ + m;
                iU_[idx] = recurrent[static_cast<std::size_t>(m)][static_cast<std::size_t>(k)];
                fU_[idx] = recurrent[static_cast<std::size_t>(m)][static_cast<std::size_t>(k + out_)];
                cU_[idx] = recurrent[static_cast<std::size_t>(m)][static_cast<std::size_t>(k + 2 * out_)];
                oU_[idx] = recurrent[static_cast<std::size_t>(m)][static_cast<std::size_t>(k + 3 * out_)];
            }
        for (int k = 0; k < out_; ++k) {
            ib_[static_cast<std::size_t>(k)] = bias[static_cast<std::size_t>(k)];
            fb_[static_cast<std::size_t>(k)] = bias[static_cast<std::size_t>(k + out_)];
            cb_[static_cast<std::size_t>(k)] = bias[static_cast<std::size_t>(k + 2 * out_)];
            ob_[static_cast<std::size_t>(k)] = bias[static_cast<std::size_t>(k + 3 * out_)];
        }
    }

    void forward(const float* x, float* out) override {
        const float* h = h_.data();
        for (int k = 0; k < out_; ++k) {
            const std::size_t wi = static_cast<std::size_t>(k) * in_;
            const std::size_t ui = static_cast<std::size_t>(k) * out_;
            const float ig = sigmoid(dot(&iW_[wi], x, in_) + dot(&iU_[ui], h, out_) + ib_[static_cast<std::size_t>(k)]);
            const float fg = sigmoid(dot(&fW_[wi], x, in_) + dot(&fU_[ui], h, out_) + fb_[static_cast<std::size_t>(k)]);
            const float og = sigmoid(dot(&oW_[wi], x, in_) + dot(&oU_[ui], h, out_) + ob_[static_cast<std::size_t>(k)]);
            const float g  = std::tanh(dot(&cW_[wi], x, in_) + dot(&cU_[ui], h, out_) + cb_[static_cast<std::size_t>(k)]);
            const float nc = fg * c_[static_cast<std::size_t>(k)] + ig * g;
            nc_[static_cast<std::size_t>(k)] = nc;
            nh_[static_cast<std::size_t>(k)] = og * std::tanh(nc);
        }
        std::copy(nc_.begin(), nc_.end(), c_.begin());
        std::copy(nh_.begin(), nh_.end(), h_.begin());
        std::copy(nh_.begin(), nh_.end(), out);
    }

private:
    static float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
    int in_, out_;
    std::vector<float> iW_, fW_, cW_, oW_, iU_, fU_, cU_, oU_;
    std::vector<float> ib_, fb_, cb_, ob_;
    std::vector<float> h_, c_, nh_, nc_;
};

// Standalone elementwise activation layer (Keras often exports these separately).
class KerasActivation final : public KerasLayer {
public:
    KerasActivation(int size, Activation act) : size_(size), act_(act) {}
    int in_size() const override { return size_; }
    int out_size() const override { return size_; }
    void forward(const float* x, float* out) override {
        for (int i = 0; i < size_; ++i) out[i] = apply_activation(act_, x[i]);
    }

private:
    int size_;
    Activation act_;
};

// Keras Dense (affine + optional activation).
class KerasDense final : public KerasLayer {
public:
    KerasDense(int in, int out, Activation act) : in_(in), out_(out), act_(act) {
        w_.assign(static_cast<std::size_t>(in) * out, 0.0f);
        b_.assign(out, 0.0f);
    }
    int in_size() const override { return in_; }
    int out_size() const override { return out_; }

    // kernel[in][out] (Keras), bias[out]. Stored transposed as w_[out][in].
    void load(const std::vector<std::vector<float>>& kernel, const std::vector<float>& bias) {
        for (int j = 0; j < out_; ++j)
            for (int i = 0; i < in_; ++i)
                w_[static_cast<std::size_t>(j) * in_ + i] =
                    kernel[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
        for (int j = 0; j < out_ && j < static_cast<int>(bias.size()); ++j)
            b_[static_cast<std::size_t>(j)] = bias[static_cast<std::size_t>(j)];
    }

    void forward(const float* x, float* out) override {
        for (int j = 0; j < out_; ++j)
            out[j] = apply_activation(act_,
                                      b_[static_cast<std::size_t>(j)]
                                          + dot(&w_[static_cast<std::size_t>(j) * in_], x, in_));
    }

private:
    int in_, out_;
    Activation act_;
    std::vector<float> w_, b_;
};

// A loaded Keras sequential model.
class KerasModel {
public:
    bool ok() const { return ok_; }
    int in_size() const { return in_size_; }
    int out_size() const { return out_size_; }

    void reset() { for (auto& l : layers_) l->reset(); }

    // Settle recurrent state at the silence steady state (off-thread).
    void prewarm() {
        reset();
        for (int i = 0; i < 4096; ++i) process_sample(0.0f);
    }

    float process_sample(float x) {
        if (layers_.empty()) return x;
        buf_a_[0] = x;
        const float* cur = buf_a_.data();
        std::vector<float>* out = &buf_a_;
        for (std::size_t i = 0; i < layers_.size(); ++i) {
            out = (i % 2 == 0) ? &buf_b_ : &buf_a_;
            layers_[i]->forward(cur, out->data());
            cur = out->data();
        }
        return (*out)[0];
    }

    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) out[i] = process_sample(in[i]);
    }

    friend bool load_keras_model(const std::string& path, KerasModel& out, std::string* error);

private:
    std::vector<std::unique_ptr<KerasLayer>> layers_;
    std::vector<float> buf_a_, buf_b_;
    int in_size_ = 1;
    int out_size_ = 1;
    bool ok_ = false;
};

namespace detail {

inline int shape_last(const choc::value::ValueView& shape) {
    if (!shape.isArray() || shape.size() == 0) return -1;
    const choc::value::ValueView last = shape[shape.size() - 1];
    if (last.isVoid()) return -1;
    return static_cast<int>(last.getWithDefault<double>(0.0));
}

inline std::vector<float> to_vec(const choc::value::ValueView& a) {
    std::vector<float> v;
    if (!a.isArray()) return v;
    v.reserve(a.size());
    for (uint32_t i = 0; i < a.size(); ++i) v.push_back(static_cast<float>(a[i].getWithDefault<double>(0.0)));
    return v;
}

inline std::vector<std::vector<float>> to_mat(const choc::value::ValueView& a) {
    std::vector<std::vector<float>> m;
    if (!a.isArray()) return m;
    m.reserve(a.size());
    for (uint32_t i = 0; i < a.size(); ++i) m.push_back(to_vec(a[i]));
    return m;
}

}  // namespace detail

// Upper bound on any single layer width — rejects a crafted `shape` that would
// otherwise drive an enormous allocation. Amp models are tens of units wide.
inline constexpr int kMaxKerasWidth = 1 << 16;

namespace detail {

// A weight matrix is well-formed iff it has exactly `rows` rows and every row has
// at least `cols` columns (extra columns, from a padded export, are ignored; too
// few would let load() read out of bounds).
inline bool mat_shape(const std::vector<std::vector<float>>& m, int rows, int cols) {
    if (static_cast<int>(m.size()) != rows) return false;
    for (const auto& r : m)
        if (static_cast<int>(r.size()) < cols) return false;
    return true;
}

}  // namespace detail

// Load an RTNeural/Keras .json model (a stack of GRU / LSTM / Dense / activation
// layers). Returns false + sets `error` on any unsupported layer, malformed
// shape, or ragged weight array; on failure `out` is left in a safe, inert state
// (a subsequent process_sample() is a pass-through, never an out-of-bounds read).
inline bool load_keras_model(const std::string& path, KerasModel& out, std::string* error) {
    // Reset to a safe, inert state up front so any early return — or a caller that
    // ignores the false return and calls process_sample() anyway — cannot touch a
    // half-built layer chain or an unsized buffer.
    out.layers_.clear();
    out.in_size_ = 1;
    out.out_size_ = 1;
    out.buf_a_.assign(1, 0.0f);
    out.buf_b_.assign(1, 0.0f);
    out.ok_ = false;
    auto fail = [&](const std::string& m) {
        out.layers_.clear();
        out.buf_a_.assign(1, 0.0f);
        out.buf_b_.assign(1, 0.0f);
        out.ok_ = false;
        if (error) *error = m;
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
    if (!root.isObject()) return fail("Keras: top-level JSON is not an object");
    if (!root.hasObjectMember("layers") || !root["layers"].isArray())
        return fail("Keras: missing 'layers' array");

    const int in_size = root.hasObjectMember("in_shape") ? detail::shape_last(root["in_shape"]) : 1;
    if (in_size <= 0) return fail("Keras: invalid 'in_shape'");
    // The runtime is scalar (process_sample takes one float): a single input
    // channel per step. Multi-input models are rejected, not silently truncated.
    if (in_size != 1) return fail("Keras: only single-input models are supported");

    const choc::value::ValueView layers = root["layers"];
    int cur_in = in_size;
    int max_width = in_size;
    for (uint32_t li = 0; li < layers.size(); ++li) {
        const std::string ix = std::to_string(li);
        const choc::value::ValueView L = layers[li];
        if (!L.isObject() || !L.hasObjectMember("type"))
            return fail("Keras: layer " + ix + " missing 'type'");
        const std::string type = std::string(L["type"].getString());
        const int out_size = L.hasObjectMember("shape") ? detail::shape_last(L["shape"]) : -1;
        if (out_size <= 0) return fail("Keras: layer " + ix + " invalid 'shape'");
        if (out_size > kMaxKerasWidth) return fail("Keras: layer " + ix + " width too large");
        const std::string act = L.hasObjectMember("activation") && L["activation"].isString()
                                    ? std::string(L["activation"].getString()) : std::string();
        const choc::value::ValueView w = L.hasObjectMember("weights") ? L["weights"] : choc::value::ValueView();

        // Strict activation parse: an unknown name (e.g. elu/softmax) is rejected,
        // never silently coerced to identity.
        auto strict_act = [&](std::string* err_out) -> Activation {
            const Activation a = parse_activation(act);
            if (!act.empty() && a == Activation::None && act != "linear")
                *err_out = "Keras: layer " + ix + " unsupported activation '" + act + "'";
            return a;
        };
        // The GRU/LSTM cell activation is fixed to tanh (the recurrent-activation
        // convention these exports use); a different value would silently mis-render.
        auto recurrent_act_ok = [&]() { return act.empty() || act == "tanh"; };

        if (type == "gru") {
            if (!w.isArray() || w.size() < 3)
                return fail("Keras: GRU layer " + ix + " needs 3 weight sets");
            if (!recurrent_act_ok())
                return fail("Keras: GRU layer " + ix + " unsupported activation '" + act + "'");
            const auto kernel = detail::to_mat(w[0]), recurrent = detail::to_mat(w[1]),
                       bias = detail::to_mat(w[2]);
            if (!detail::mat_shape(kernel, cur_in, 3 * out_size)
                || !detail::mat_shape(recurrent, out_size, 3 * out_size)
                || !detail::mat_shape(bias, 2, 3 * out_size))
                return fail("Keras: GRU layer " + ix + " weight shape mismatch");
            auto g = std::make_unique<KerasGru>(cur_in, out_size);
            g->load(kernel, recurrent, bias);
            out.layers_.push_back(std::move(g));
        } else if (type == "lstm") {
            if (!w.isArray() || w.size() < 3)
                return fail("Keras: LSTM layer " + ix + " needs 3 weight sets");
            if (!recurrent_act_ok())
                return fail("Keras: LSTM layer " + ix + " unsupported activation '" + act + "'");
            const auto kernel = detail::to_mat(w[0]), recurrent = detail::to_mat(w[1]);
            const auto bias = detail::to_vec(w[2]);
            if (!detail::mat_shape(kernel, cur_in, 4 * out_size)
                || !detail::mat_shape(recurrent, out_size, 4 * out_size)
                || static_cast<int>(bias.size()) < 4 * out_size)
                return fail("Keras: LSTM layer " + ix + " weight shape mismatch");
            auto l = std::make_unique<KerasLstm>(cur_in, out_size);
            l->load(kernel, recurrent, bias);
            out.layers_.push_back(std::move(l));
        } else if (type == "activation") {
            if (out_size != cur_in)
                return fail("Keras: activation layer " + ix + " changes width");
            std::string aerr;
            const Activation a = strict_act(&aerr);
            if (!aerr.empty()) return fail(aerr);
            out.layers_.push_back(std::make_unique<KerasActivation>(cur_in, a));
        } else if (type == "dense" || type == "time-distributed-dense") {
            if (!w.isArray() || w.size() < 1)
                return fail("Keras: Dense layer " + ix + " needs weights");
            std::string aerr;
            const Activation a = strict_act(&aerr);
            if (!aerr.empty()) return fail(aerr);
            const auto kernel = detail::to_mat(w[0]);
            if (!detail::mat_shape(kernel, cur_in, out_size))
                return fail("Keras: Dense layer " + ix + " kernel shape mismatch");
            std::vector<float> bias;
            if (w.size() >= 2) {
                bias = detail::to_vec(w[1]);
                if (static_cast<int>(bias.size()) < out_size)
                    return fail("Keras: Dense layer " + ix + " bias too short");
            }
            auto d = std::make_unique<KerasDense>(cur_in, out_size, a);
            d->load(kernel, bias);
            out.layers_.push_back(std::move(d));
        } else {
            return fail("Keras: unsupported layer type '" + type + "'");
        }
        cur_in = out_size;
        if (out_size > max_width) max_width = out_size;
    }

    if (out.layers_.empty()) return fail("Keras: no layers");
    // The scalar API returns one output per step, so the final layer must be
    // width 1; a wider tail would be silently truncated.
    if (cur_in != 1) return fail("Keras: model output must be a single channel");
    out.in_size_ = in_size;
    out.out_size_ = cur_in;
    out.buf_a_.assign(static_cast<std::size_t>(max_width), 0.0f);
    out.buf_b_.assign(static_cast<std::size_t>(max_width), 0.0f);
    out.reset();
    out.ok_ = true;
    return true;
}

}  // namespace pulp::examples::keras
