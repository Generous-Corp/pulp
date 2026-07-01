#pragma once

// NAM ConvNet architecture CPU inference + a ``.nam`` loader.
//
// ConvNet is a straightforward feedforward stack: a sequence of blocks, each a
// dilated causal convolution (kernel size 2) followed by an optional batch-norm
// and a shared activation, then a linear 1x1 head mapping the channel vector to
// the output. Unlike WaveNet there is no skip/residual and no head_scale.
//
// It reuses nam_model.hpp's ``Conv`` primitive (weight layout ``[out][in][k]``
// then bias) for both the block convolutions and the head, so ConvNet shares the
// exact, A1-validated conv math. Grouped convolutions are rejected (real captures
// use groups == 1).
//
// The flat ``weights`` array is consumed in this order:
//   for each block: conv([in->channels][k=2] + bias unless batchnorm),
//                   batchnorm(mean, var, weight, bias, eps)   [only if batchnorm]
//   head([channels->out_channels][k=1] + bias)

#include "nam_model.hpp"   // Conv, Activation, parse_activation, apply_activation

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::nam {

// Batch-norm in inference mode is a per-channel affine map. From the stored
// (mean, var, weight, bias, eps) it precomputes y = scale*x + loc, with
// scale = weight/sqrt(eps+var), loc = bias - scale*mean. Weights are consumed as
// mean(dim), var(dim), weight(dim), bias(dim), eps(1).
class ConvNetBatchNorm {
public:
    void configure(int dim) {
        dim_ = dim;
        scale_.assign(static_cast<std::size_t>(dim), 0.0f);
        loc_.assign(static_cast<std::size_t>(dim), 0.0f);
    }
    std::size_t weight_count() const { return 4 * static_cast<std::size_t>(dim_) + 1; }
    void load(const float*& p) {
        std::vector<float> mean(dim_), var(dim_), w(dim_), b(dim_);
        for (int i = 0; i < dim_; ++i) mean[static_cast<std::size_t>(i)] = *p++;
        for (int i = 0; i < dim_; ++i) var[static_cast<std::size_t>(i)] = *p++;
        for (int i = 0; i < dim_; ++i) w[static_cast<std::size_t>(i)] = *p++;
        for (int i = 0; i < dim_; ++i) b[static_cast<std::size_t>(i)] = *p++;
        const float eps = *p++;
        for (int i = 0; i < dim_; ++i) {
            const auto s = static_cast<std::size_t>(i);
            scale_[s] = w[s] / std::sqrt(eps + var[s]);
            loc_[s] = b[s] - scale_[s] * mean[s];
        }
    }
    void apply(float* x) const {
        for (int i = 0; i < dim_; ++i)
            x[static_cast<std::size_t>(i)] = scale_[static_cast<std::size_t>(i)] * x[static_cast<std::size_t>(i)]
                                             + loc_[static_cast<std::size_t>(i)];
    }

private:
    int dim_ = 1;
    std::vector<float> scale_, loc_;
};

// One ConvNet block: dilated conv (kernel 2) -> optional batch-norm -> activation.
class ConvNetBlock {
public:
    void configure(int in_ch, int channels, int dilation, bool batchnorm, Activation act) {
        channels_ = channels;
        batchnorm_ = batchnorm;
        act_ = act;
        conv_.configure(in_ch, channels, /*kernel=*/2, dilation, /*bias=*/!batchnorm);
        if (batchnorm) bn_.configure(channels);
    }
    std::size_t weight_count() const {
        return conv_.weight_count() + (batchnorm_ ? bn_.weight_count() : 0);
    }
    void load(const float*& p) {
        conv_.load(p);
        if (batchnorm_) bn_.load(p);
    }
    void reset() { conv_.reset(); }
    void step(const float* x, float* out) {
        conv_.step(x, out);
        if (batchnorm_) bn_.apply(out);
        for (int i = 0; i < channels_; ++i)
            out[static_cast<std::size_t>(i)] = apply_activation(act_, out[static_cast<std::size_t>(i)]);
    }
    int channels() const { return channels_; }
    int dilation() const { return dilation_; }

private:
    int channels_ = 1;
    int dilation_ = 1;
    bool batchnorm_ = false;
    Activation act_ = Activation::ReLU;
    Conv conv_;
    ConvNetBatchNorm bn_;
};

// Full ConvNet model: a block stack + a linear 1x1 head.
class NamConvNet {
public:
    bool build(int in_channels, int out_channels, int channels, std::vector<int> dilations,
               bool batchnorm, Activation act, std::vector<float> weights, double sample_rate,
               std::string* error = nullptr) {
        auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
        if (in_channels != 1) return fail("ConvNet: only in_channels == 1 is supported");
        if (out_channels != 1) return fail("ConvNet: only out_channels == 1 is supported");
        if (channels <= 0) return fail("ConvNet: channels must be positive");
        if (dilations.empty()) return fail("ConvNet: 'dilations' must be non-empty");

        channels_ = channels;
        out_channels_ = out_channels;
        dilations_ = dilations;
        blocks_.clear();
        blocks_.resize(dilations.size());
        std::size_t expected = 0;
        for (std::size_t i = 0; i < dilations.size(); ++i) {
            const int in_ch = i == 0 ? in_channels : channels;
            blocks_[i].configure(in_ch, channels, dilations[i], batchnorm, act);
            expected += blocks_[i].weight_count();
        }
        head_.configure(channels, out_channels, /*kernel=*/1, /*dilation=*/1, /*bias=*/true);
        expected += head_.weight_count();
        if (weights.size() != expected)
            return fail("ConvNet weight count mismatch: file provides " + std::to_string(weights.size())
                        + ", model layout consumes " + std::to_string(expected));

        sample_rate_ = sample_rate;
        buf_a_.assign(static_cast<std::size_t>(channels), 0.0f);
        buf_b_.assign(static_cast<std::size_t>(channels), 0.0f);
        head_out_.assign(static_cast<std::size_t>(out_channels), 0.0f);
        const float* p = weights.data();
        for (ConvNetBlock& b : blocks_) b.load(p);
        head_.load(p);
        reset();
        ok_ = (static_cast<std::size_t>(p - weights.data()) == weights.size());
        return ok_;
    }

    void reset() {
        for (ConvNetBlock& b : blocks_) b.reset();
        head_.reset();
    }

    long receptive_field() const {
        long rf = 0;
        for (int d : dilations_) rf += d;   // kernel 2 -> reach = dilation per block
        return rf;
    }

    void prewarm() {
        reset();
        long warm = 2 * receptive_field() + 512;
        if (warm > (1 << 18)) warm = 1 << 18;
        for (long i = 0; i < warm; ++i) process_sample(0.0f);
    }

    float process_sample(float x) {
        const float in[1] = {x};
        const float* cur = in;
        std::vector<float>* out = &buf_a_;
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            out = (i % 2 == 0) ? &buf_a_ : &buf_b_;
            blocks_[i].step(cur, out->data());
            cur = out->data();
        }
        head_.step(cur, head_out_.data());
        return head_out_[0];
    }

    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) out[i] = process_sample(in[i]);
    }

    bool ok() const { return ok_; }
    double sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }

private:
    std::vector<ConvNetBlock> blocks_;
    Conv head_;
    std::vector<int> dilations_;
    int channels_ = 1;
    int out_channels_ = 1;
    double sample_rate_ = -1.0;
    bool ok_ = false;
    std::vector<float> buf_a_, buf_b_, head_out_;
};

// Load a ConvNet .nam from a file. Rejects grouped convolutions and any
// non-mono in/out (real captures are mono, groups == 1).
inline bool load_nam_convnet(const std::string& path, NamConvNet& out, std::string* error = nullptr) {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
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
    const std::string arch =
        root.hasObjectMember("architecture") ? std::string(root["architecture"].getString()) : std::string();
    if (arch != "ConvNet") return fail("unsupported architecture: '" + arch + "' (expected ConvNet)");
    if (!root.hasObjectMember("config")) return fail("ConvNet: missing 'config'");
    const choc::value::ValueView config = root["config"];

    const double sr = root.hasObjectMember("sample_rate") ? detail::num(root["sample_rate"]) : -1.0;
    const int in_channels = config.hasObjectMember("in_channels")
                                ? static_cast<int>(detail::num(config["in_channels"])) : 1;
    const int out_channels = config.hasObjectMember("out_channels")
                                 ? static_cast<int>(detail::num(config["out_channels"])) : 1;
    if (config.hasObjectMember("groups") && static_cast<int>(detail::num(config["groups"])) != 1)
        return fail("ConvNet: grouped conv not supported");
    if (!config.hasObjectMember("channels")) return fail("ConvNet: missing 'channels'");
    const int channels = static_cast<int>(detail::num(config["channels"]));
    const bool batchnorm = config.hasObjectMember("batchnorm") && config["batchnorm"].getWithDefault<bool>(false);

    if (!config.hasObjectMember("dilations") || !config["dilations"].isArray())
        return fail("ConvNet: 'dilations' must be an array");
    const choc::value::ValueView dil = config["dilations"];
    std::vector<int> dilations;
    for (uint32_t i = 0; i < dil.size(); ++i) {
        const int dv = static_cast<int>(detail::num(dil[i]));
        if (dv <= 0 || dv > (1 << 16)) return fail("ConvNet: dilation out of range");
        dilations.push_back(dv);
    }

    // activation: a bare string ("Tanh"/"ReLU"/...) or an object with "type".
    std::string act_name = "Tanh";
    if (config.hasObjectMember("activation")) {
        const choc::value::ValueView av = config["activation"];
        if (av.isString()) act_name = std::string(av.getString());
        else if (av.isObject() && av.hasObjectMember("type") && av["type"].isString())
            act_name = std::string(av["type"].getString());
    }
    const Activation act = parse_activation(act_name);

    if (!root.hasObjectMember("weights")) return fail("ConvNet: missing 'weights'");
    const choc::value::ValueView wv = root["weights"];
    if (!wv.isArray()) return fail("ConvNet: 'weights' must be an array");
    std::vector<float> weights;
    weights.reserve(wv.size());
    for (uint32_t i = 0; i < wv.size(); ++i) {
        const double wd = detail::num(wv[i]);
        if (!std::isfinite(wd)) return fail("ConvNet: non-finite weight");
        weights.push_back(static_cast<float>(wd));
    }

    return out.build(in_channels, out_channels, channels, std::move(dilations), batchnorm, act,
                     std::move(weights), sr, error);
}

}  // namespace pulp::examples::nam
