#pragma once

// NAM Architecture 2 (A2) CPU inference + a ``.nam`` loader.
//
// A2 is a WaveNet variant: the same dilated causal-conv stack as A1, with four
// differences that this file models exactly:
//   1. per-layer kernel sizes (config.kernel_sizes is an array, not a scalar),
//   2. per-layer LeakyReLU activations (config.activation is an array of
//      {type, negative_slope} objects),
//   3. a windowed convolutional head — config.head = {out_channels, kernel_size,
//      bias} — over the skip accumulator, replacing A1's per-sample 1x1 head, and
//   4. a top-level "SlimmableContainer" holding several size variants (A2-Full,
//      A2-Lite, ...) selected at run time by a "size" value in [0, 1].
//
// It reuses nam_model.hpp's ``Conv`` primitive (dilated causal conv + history
// ring, weight layout ``[out][in][k]`` then bias), so A2 shares the exact,
// A1-validated conv math — only the wiring differs. Published A2 captures leave
// FiLM / gating / head1x1 / grouped conv / secondary activation OFF and set
// bottleneck == channels and a single layer array; those shapes are rejected at
// load time rather than silently mis-rendered.
//
// The flat per-submodel ``weights`` array is consumed in this order (matching the
// layer-array/layer/conv weight ordering, same as A1):
//   rechannel(input->channels, 1x1, no bias)
//   for each layer: conv([channels->channels][kernel]+bias),
//                   input_mixin([condition->channels][1], no bias),
//                   layer1x1([channels->channels][1]+bias)
//   head(bottleneck->out_channels, kernel, +bias)
//   head_scale   (one trailing scalar)

#include "nam_model.hpp"   // Conv

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::nam {

// A2 activation. LeakyReLU is the A2 primary; the others are accepted for
// robustness. LeakyReLU(x) = x >= 0 ? x : slope*x.
struct A2Act {
    enum Kind { LeakyReLU, Tanh, ReLU, Sigmoid, Hardtanh, Identity };
    Kind kind = LeakyReLU;
    float slope = 0.01f;  // LeakyReLU negative slope

    float apply(float x) const {
        switch (kind) {
            case LeakyReLU: return x >= 0.0f ? x : slope * x;
            case Tanh:      return std::tanh(x);
            case ReLU:      return x > 0.0f ? x : 0.0f;
            case Sigmoid:   return 1.0f / (1.0f + std::exp(-x));
            case Hardtanh:  return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
            default:        return x;
        }
    }
};

// One A2 WaveNet layer: dilated conv + input-mixin + activation + 1x1 residual.
// bottleneck == channels (enforced at load), gating OFF: z = channels.
class A2Layer {
public:
    void configure(int channels, int condition_size, int bottleneck, int kernel,
                   int dilation, A2Act act) {
        channels_ = channels;
        act_ = act;
        const int z = bottleneck;                       // == channels for real A2
        conv_.configure(channels, z, kernel, dilation, /*bias=*/true);
        input_mixin_.configure(condition_size, z, /*kernel=*/1, /*dilation=*/1, /*bias=*/false);
        layer1x1_.configure(z, channels, /*kernel=*/1, /*dilation=*/1, /*bias=*/true);
        z_.assign(static_cast<std::size_t>(z), 0.0f);
        mix_.assign(static_cast<std::size_t>(z), 0.0f);
        l1_.assign(static_cast<std::size_t>(channels), 0.0f);
    }

    std::size_t weight_count() const {
        return conv_.weight_count() + input_mixin_.weight_count() + layer1x1_.weight_count();
    }
    void load(const float*& p) { conv_.load(p); input_mixin_.load(p); layer1x1_.load(p); }
    void reset() { conv_.reset(); input_mixin_.reset(); layer1x1_.reset(); }

    // input: channels, condition: condition_size, out_next: channels (residual),
    // out_head: z (skip, bottleneck==channels).
    void step(const float* input, const float* condition, float* out_next, float* out_head) {
        conv_.step(input, z_.data());
        input_mixin_.step(condition, mix_.data());
        const int z = static_cast<int>(z_.size());
        for (int i = 0; i < z; ++i) z_[static_cast<std::size_t>(i)] += mix_[static_cast<std::size_t>(i)];
        for (int i = 0; i < z; ++i)
            z_[static_cast<std::size_t>(i)] = act_.apply(z_[static_cast<std::size_t>(i)]);
        for (int i = 0; i < z; ++i) out_head[i] = z_[static_cast<std::size_t>(i)];
        layer1x1_.step(z_.data(), l1_.data());
        for (int c = 0; c < channels_; ++c) out_next[c] = input[c] + l1_[static_cast<std::size_t>(c)];
    }

private:
    int channels_ = 1;
    A2Act act_;
    Conv conv_, input_mixin_, layer1x1_;
    std::vector<float> z_, mix_, l1_;
};

// Per-layer-array config for A2 (mixed kernels + per-layer activations + windowed
// conv head). condition == raw input (condition_size == 1).
struct A2ArrayConfig {
    int input_size = 1;
    int condition_size = 1;
    int channels = 1;
    int bottleneck = 1;                 // == channels
    std::vector<int> kernel_sizes;      // per layer
    std::vector<int> dilations;         // per layer (same length)
    std::vector<A2Act> activations;     // per layer (same length)
    int head_out = 1;                   // head out_channels
    int head_kernel = 1;                // windowed head kernel (>1 in A2)
    bool head_bias = true;
};

// A chain of A2 layers with a windowed convolutional head: rechannel -> layers ->
// head-conv over the skip accumulator. A2 captures use a single such array.
class A2LayerArray {
public:
    void configure(const A2ArrayConfig& cfg) {
        cfg_ = cfg;
        rechannel_.configure(cfg.input_size, cfg.channels, 1, 1, /*bias=*/false);
        layers_.clear();
        layers_.resize(cfg.dilations.size());
        for (std::size_t i = 0; i < cfg.dilations.size(); ++i)
            layers_[i].configure(cfg.channels, cfg.condition_size, cfg.bottleneck,
                                 cfg.kernel_sizes[i], cfg.dilations[i], cfg.activations[i]);
        head_.configure(cfg.bottleneck, cfg.head_out, cfg.head_kernel, 1, /*bias=*/cfg.head_bias);
        rc_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
        acc_.assign(static_cast<std::size_t>(cfg.bottleneck), 0.0f);
        cur_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
        next_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
        skip_.assign(static_cast<std::size_t>(cfg.bottleneck), 0.0f);
    }

    std::size_t weight_count() const {
        std::size_t n = rechannel_.weight_count();
        for (const A2Layer& l : layers_) n += l.weight_count();
        return n + head_.weight_count();
    }
    void load(const float*& p) {
        rechannel_.load(p);
        for (A2Layer& l : layers_) l.load(p);
        head_.load(p);
    }
    void reset() {
        rechannel_.reset();
        for (A2Layer& l : layers_) l.reset();
        head_.reset();
    }

    // layer_inputs: input_size, condition: condition_size, layer_out: channels,
    // head_out: head_out_channels.
    void step(const float* layer_inputs, const float* condition, float* layer_out, float* head_out) {
        rechannel_.step(layer_inputs, rc_.data());
        const int C = cfg_.channels, B = cfg_.bottleneck;
        for (int i = 0; i < B; ++i) acc_[static_cast<std::size_t>(i)] = 0.0f;
        for (int i = 0; i < C; ++i) cur_[static_cast<std::size_t>(i)] = rc_[static_cast<std::size_t>(i)];
        for (std::size_t l = 0; l < layers_.size(); ++l) {
            layers_[l].step(cur_.data(), condition, next_.data(), skip_.data());
            for (int i = 0; i < B; ++i) acc_[static_cast<std::size_t>(i)] += skip_[static_cast<std::size_t>(i)];
            for (int i = 0; i < C; ++i) cur_[static_cast<std::size_t>(i)] = next_[static_cast<std::size_t>(i)];
        }
        for (int i = 0; i < C; ++i) layer_out[i] = cur_[static_cast<std::size_t>(i)];
        head_.step(acc_.data(), head_out);
    }

    int channels() const { return cfg_.channels; }
    int head_out() const { return cfg_.head_out; }
    long receptive_field() const {
        long rf = 0;
        for (std::size_t i = 0; i < cfg_.dilations.size(); ++i)
            rf += static_cast<long>(cfg_.kernel_sizes[i] - 1) * cfg_.dilations[i];
        rf += static_cast<long>(cfg_.head_kernel - 1);
        return rf;
    }

private:
    A2ArrayConfig cfg_;
    Conv rechannel_;
    std::vector<A2Layer> layers_;
    Conv head_;
    std::vector<float> rc_, acc_, cur_, next_, skip_;
};

// One A2 WaveNet size variant: a single layer array + a trailing head_scale.
class A2Model {
public:
    // weights includes the trailing head_scale scalar (the .nam layout).
    bool build(A2ArrayConfig arr, std::vector<float> weights, double sample_rate,
               std::string* error = nullptr) {
        auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
        array_.configure(arr);
        const std::size_t expected = array_.weight_count() + 1;   // +1 head_scale
        if (weights.size() != expected)
            return fail("A2 weight count mismatch: file provides " + std::to_string(weights.size())
                        + ", model layout consumes " + std::to_string(expected));
        weights_ = std::move(weights);
        sample_rate_ = sample_rate;
        head_out_ = arr.head_out;
        head_buf_.assign(static_cast<std::size_t>(head_out_), 0.0f);
        channels_ = arr.channels;
        layer_scratch_.assign(static_cast<std::size_t>(channels_), 0.0f);  // RT: never alloc in process
        const float* p = weights_.data();
        array_.load(p);
        head_scale_ = *p++;
        reset();
        ok_ = (static_cast<std::size_t>(p - weights_.data()) == weights_.size());
        return ok_;
    }

    void reset() { array_.reset(); }
    long receptive_field() const { return array_.receptive_field(); }

    void prewarm() {
        reset();
        long warm = 2 * receptive_field() + 512;
        if (warm > (1 << 18)) warm = 1 << 18;
        for (long i = 0; i < warm; ++i) process_sample(0.0f);
    }

    float process_sample(float x) {
        const float in[1] = {x};
        const float cond[1] = {x};
        array_.step(in, cond, layer_scratch(), head_buf_.data());
        return head_buf_[0] * head_scale_;
    }

    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) out[i] = process_sample(in[i]);
    }

    bool ok() const { return ok_; }
    double sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }
    float head_scale() const { return head_scale_; }

private:
    float* layer_scratch() {
        if (static_cast<int>(layer_scratch_.size()) < channels_)
            layer_scratch_.assign(static_cast<std::size_t>(channels_), 0.0f);
        return layer_scratch_.data();
    }

    A2LayerArray array_;
    std::vector<float> weights_;
    float head_scale_ = 1.0f;
    double sample_rate_ = -1.0;
    int head_out_ = 1;
    int channels_ = 1;
    bool ok_ = false;
    std::vector<float> head_buf_, layer_scratch_;
};

namespace detail_a2 {

// Parse a JSON value as an integer in [lo, hi]. Sets ok=false (never flips it back
// to true) on anything that is not a finite, integral number in range — including
// strings/objects/arrays/bools, NaN/Inf, fractional values, and out-of-range
// magnitudes like 1e100 that would be undefined to cast to int. Returns lo on
// failure so downstream arithmetic stays bounded.
inline long parse_int(const choc::value::ValueView& v, long lo, long hi, bool& ok) {
    if (v.isVoid() || v.isObject() || v.isArray() || v.isString() || v.isBool()) {
        ok = false;
        return lo;
    }
    const double d = detail::num(v);
    if (!std::isfinite(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi)
        || d != std::floor(d)) {
        ok = false;
        return lo;
    }
    return static_cast<long>(d);
}

// Parse one activation entry: {"type":"LeakyReLU","negative_slope":0.01} or a bare
// string name. Returns false on an unknown/malformed type or a non-finite slope, so
// a typo like "LeakyyReLU" fails the load instead of silently rendering Identity.
inline bool parse_act(const choc::value::ValueView& v, A2Act& out) {
    A2Act a;
    std::string type;
    if (v.isString()) {
        type = std::string(v.getString());
    } else if (v.isObject() && v.hasObjectMember("type") && v["type"].isString()) {
        type = std::string(v["type"].getString());
        if (v.hasObjectMember("negative_slope")) {
            const double s = detail::num(v["negative_slope"]);
            if (!std::isfinite(s)) return false;
            a.slope = static_cast<float>(s);
        }
    } else {
        return false;
    }
    if (type == "LeakyReLU")                     a.kind = A2Act::LeakyReLU;
    else if (type == "Tanh")                     a.kind = A2Act::Tanh;
    else if (type == "ReLU")                     a.kind = A2Act::ReLU;
    else if (type == "Sigmoid")                  a.kind = A2Act::Sigmoid;
    else if (type == "Hardtanh")                 a.kind = A2Act::Hardtanh;
    else if (type == "Linear" || type == "Identity") a.kind = A2Act::Identity;
    else return false;  // unknown activation -> reject, never silently mis-render
    out = a;
    return true;
}

// True iff any layer array carries A2's distinguishing markers (per-layer
// kernel_sizes array, a windowed head dict, or a per-layer activation array).
// Scans every array — a multi-array file with A2 markers only in a later array
// must still route to A2 (or be rejected there) rather than to the A1 loader.
inline bool looks_like_a2(const choc::value::ValueView& config) {
    if (!config.isObject() || !config.hasObjectMember("layers")) return false;
    const choc::value::ValueView layers = config["layers"];
    if (!layers.isArray() || layers.size() == 0) return false;
    for (uint32_t i = 0; i < layers.size(); ++i) {
        const choc::value::ValueView L = layers[i];
        if (!L.isObject()) continue;
        if (L.hasObjectMember("kernel_sizes") && L["kernel_sizes"].isArray()) return true;
        if (L.hasObjectMember("head") && L["head"].isObject()) return true;
        if (L.hasObjectMember("activation") && L["activation"].isArray()) return true;
    }
    return false;
}

// Parse a single A2 WaveNet submodel's config.layers[0] + weights + head_scale.
// Rejects multi-array models and every advanced knob that is actually active.
inline bool parse_submodel(const choc::value::ValueView& model, A2ArrayConfig& arr_out,
                           std::vector<float>& weights_out, double& sr_out, std::string* error) {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    if (!model.isObject()) return fail("A2: submodel is not an object");
    const std::string arch =
        model.hasObjectMember("architecture") ? std::string(model["architecture"].getString()) : std::string();
    if (arch != "WaveNet") return fail("A2: submodel architecture '" + arch + "' (expected WaveNet)");
    if (!model.hasObjectMember("config")) return fail("A2: submodel missing 'config'");
    const choc::value::ValueView config = model["config"];
    sr_out = model.hasObjectMember("sample_rate") ? detail::num(model["sample_rate"]) : sr_out;

    if (!config.hasObjectMember("layers")) return fail("A2: missing 'config.layers'");
    const choc::value::ValueView layers = config["layers"];
    if (!layers.isArray() || layers.size() == 0) return fail("A2: 'config.layers' must be non-empty");
    if (layers.size() != 1) return fail("A2: multi-array WaveNet is not supported (single array expected)");

    const choc::value::ValueView L = layers[0];
    A2ArrayConfig a;
    bool ok = true;
    // parse_int with lo=1 enforces strictly-positive dimensions and finite/in-range
    // values, so channels:0, channels:1e100, and channels:"x" all fail cleanly
    // instead of building empty/degenerate convs.
    a.input_size = static_cast<int>(parse_int(L["input_size"], 1, 1 << 16, ok));
    a.condition_size = static_cast<int>(parse_int(L["condition_size"], 1, 1 << 16, ok));
    a.channels = static_cast<int>(parse_int(L["channels"], 1, 1 << 16, ok));
    a.bottleneck = L.hasObjectMember("bottleneck")
                       ? static_cast<int>(parse_int(L["bottleneck"], 1, 1 << 16, ok)) : a.channels;
    if (!ok) return fail("A2: missing or out-of-range layer dimension "
                         "(input_size/condition_size/channels/bottleneck)");

    if (a.input_size != 1) return fail("A2: input_size != 1 not supported");
    if (a.condition_size != 1)
        return fail("A2: condition_size != 1 (parametric/conditioned model) not supported");
    if (a.bottleneck != a.channels) return fail("A2: bottleneck != channels not supported");

    // Reject advanced knobs only when actually active, so real captures (which set
    // them present-but-off) load while non-standard shapes are refused.
    auto active = [&](const char* key) {
        return L.hasObjectMember(key) && L[key].isObject() && L[key].hasObjectMember("active")
               && L[key]["active"].getWithDefault<bool>(false);
    };
    if (active("head1x1")) return fail("A2: head1x1 not supported");
    if (L.hasObjectMember("layer1x1") && L["layer1x1"].isObject()
        && L["layer1x1"].hasObjectMember("active")
        && !L["layer1x1"]["active"].getWithDefault<bool>(true))
        return fail("A2: inactive layer1x1 not supported");
    for (const char* k : {"groups_input", "groups_input_mixin"})
        if (L.hasObjectMember(k) && static_cast<int>(detail::num(L[k])) != 1)
            return fail("A2: grouped conv not supported");
    for (const char* k : {"conv_pre_film", "conv_post_film", "input_mixin_pre_film",
                          "input_mixin_post_film", "activation_pre_film", "activation_post_film",
                          "layer1x1_post_film", "head1x1_post_film"})
        if (active(k)) return fail(std::string("A2: FiLM (") + k + ") not supported");

    // kernel_sizes: per-layer array (A2) or a scalar kernel_size (fallback).
    std::vector<int> kernels;
    if (L.hasObjectMember("kernel_sizes") && L["kernel_sizes"].isArray()) {
        const choc::value::ValueView ks = L["kernel_sizes"];
        for (uint32_t i = 0; i < ks.size(); ++i) {
            bool kok = true;
            const int kv = static_cast<int>(parse_int(ks[i], 1, 1 << 12, kok));
            if (!kok) return fail("A2: kernel size out of range");
            kernels.push_back(kv);
        }
    } else if (!L.hasObjectMember("kernel_size")) {
        return fail("A2: missing kernel_sizes/kernel_size");
    }

    if (!L.hasObjectMember("dilations") || !L["dilations"].isArray())
        return fail("A2: 'dilations' must be an array");
    const choc::value::ValueView dil = L["dilations"];
    for (uint32_t d = 0; d < dil.size(); ++d) {
        bool dok = true;
        const int dv = static_cast<int>(parse_int(dil[d], 1, 1 << 16, dok));
        if (!dok) return fail("A2: dilation out of range");
        a.dilations.push_back(dv);
    }
    if (a.dilations.empty()) return fail("A2: 'dilations' must be non-empty");

    if (kernels.empty()) {  // scalar kernel_size applied to every layer
        bool kok = true;
        const int k = static_cast<int>(parse_int(L["kernel_size"], 1, 1 << 12, kok));
        if (!kok) return fail("A2: kernel size out of range");
        kernels.assign(a.dilations.size(), k);
    }
    if (kernels.size() != a.dilations.size())
        return fail("A2: kernel_sizes length (" + std::to_string(kernels.size())
                    + ") != dilations length (" + std::to_string(a.dilations.size()) + ")");
    a.kernel_sizes = std::move(kernels);

    // Per-layer activation array, a single object/string, or absent (default). An
    // unknown or malformed activation is rejected, never coerced to Identity.
    if (L.hasObjectMember("activation") && L["activation"].isArray()) {
        const choc::value::ValueView av = L["activation"];
        if (av.size() != a.dilations.size())
            return fail("A2: activation array length != layer count");
        for (uint32_t i = 0; i < av.size(); ++i) {
            A2Act one;
            if (!parse_act(av[i], one)) return fail("A2: unknown or malformed activation");
            a.activations.push_back(one);
        }
    } else if (L.hasObjectMember("activation")) {
        A2Act one;
        if (!parse_act(L["activation"], one)) return fail("A2: unknown or malformed activation");
        a.activations.assign(a.dilations.size(), one);
    } else {
        a.activations.assign(a.dilations.size(), A2Act{});  // default LeakyReLU(0.01)
    }

    // Reject any active secondary activation / gating (present-but-off is fine).
    if (L.hasObjectMember("secondary_activation") && L["secondary_activation"].isArray()) {
        const choc::value::ValueView sv = L["secondary_activation"];
        for (uint32_t i = 0; i < sv.size(); ++i)
            if (!sv[i].isVoid()) return fail("A2: secondary_activation not supported");
    } else if (L.hasObjectMember("secondary_activation") && !L["secondary_activation"].isVoid()) {
        return fail("A2: secondary_activation not supported");
    }
    if (L.hasObjectMember("gating_mode") && L["gating_mode"].isArray()) {
        const choc::value::ValueView gv = L["gating_mode"];
        for (uint32_t i = 0; i < gv.size(); ++i)
            if (gv[i].isString() && std::string(gv[i].getString()) != "none")
                return fail("A2: gating is not supported");
    }

    // Windowed head: {out_channels, kernel_size, bias}. Absent -> 1x1 head.
    bool hok = true;
    if (L.hasObjectMember("head") && L["head"].isObject()) {
        const choc::value::ValueView h = L["head"];
        a.head_out = h.hasObjectMember("out_channels")
                         ? static_cast<int>(parse_int(h["out_channels"], 1, 1 << 16, hok)) : 1;
        a.head_kernel = h.hasObjectMember("kernel_size")
                            ? static_cast<int>(parse_int(h["kernel_size"], 1, 1 << 12, hok)) : 1;
        a.head_bias = !h.hasObjectMember("bias") || h["bias"].getWithDefault<bool>(true);
    } else {
        a.head_out = L.hasObjectMember("head_size")
                         ? static_cast<int>(parse_int(L["head_size"], 1, 1 << 16, hok)) : 1;
        a.head_kernel = 1;
        a.head_bias = L.hasObjectMember("head_bias") && L["head_bias"].getWithDefault<bool>(false);
    }
    if (!hok) return fail("A2: head out_channels/kernel_size out of range");
    if (a.head_out != 1) return fail("A2: head out_channels != 1 not supported");

    // weights (per submodel, includes the trailing head_scale). Reject non-finite
    // entries so a NaN/Inf weight can't poison the whole output.
    if (!model.hasObjectMember("weights")) return fail("A2: submodel missing 'weights'");
    const choc::value::ValueView wv = model["weights"];
    if (!wv.isArray()) return fail("A2: 'weights' must be an array");
    weights_out.clear();
    weights_out.reserve(wv.size());
    for (uint32_t i = 0; i < wv.size(); ++i) {
        const double wd = detail::num(wv[i]);
        if (!std::isfinite(wd)) return fail("A2: non-finite weight");
        weights_out.push_back(static_cast<float>(wd));
    }

    arr_out = std::move(a);
    return true;
}

}  // namespace detail_a2

// An A2 model: one or more size variants (A2-Full / A2-Lite ...) selectable at
// run time. A bare A2 WaveNet loads as a single variant.
class NamA2 {
public:
    // Build from an already-parsed root object (architecture SlimmableContainer or
    // an A2-shaped WaveNet).
    bool build(const choc::value::ValueView& root, std::string* error = nullptr) {
        auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
        variants_.clear();
        active_ = 0;
        if (!root.isObject()) return fail("A2: top-level JSON is not an object");
        sr_ = root.hasObjectMember("sample_rate") ? detail::num(root["sample_rate"]) : -1.0;
        const std::string arch =
            root.hasObjectMember("architecture") ? std::string(root["architecture"].getString()) : std::string();

        if (arch == "SlimmableContainer") {
            if (!root.hasObjectMember("config") || !root["config"].isObject())
                return fail("A2: SlimmableContainer missing 'config'");
            const choc::value::ValueView cfg = root["config"];
            if (!cfg.hasObjectMember("submodels") || !cfg["submodels"].isArray())
                return fail("A2: SlimmableContainer missing 'config.submodels'");
            const choc::value::ValueView subs = cfg["submodels"];
            if (subs.size() == 0) return fail("A2: no submodels");
            for (uint32_t i = 0; i < subs.size(); ++i) {
                const choc::value::ValueView s = subs[i];
                if (!s.isObject() || !s.hasObjectMember("model"))
                    return fail("A2: submodel " + std::to_string(i) + " missing 'model'");
                const double mv = s.hasObjectMember("max_value") ? detail::num(s["max_value"]) : 1.0;
                if (!std::isfinite(mv)) return fail("A2: submodel " + std::to_string(i) + " non-finite max_value");
                if (!add_variant(s["model"], mv, error)) return false;
            }
        } else {
            // A bare A2 WaveNet -> a single full-size variant.
            if (!add_variant(root, 1.0, error)) return false;
        }
        if (variants_.empty()) return fail("A2: produced no variants");
        // Sort ascending by max_value; default to the largest (Full).
        for (std::size_t i = 1; i < variants_.size(); ++i)
            for (std::size_t j = i; j > 0 && variants_[j].max_value < variants_[j - 1].max_value; --j)
                std::swap(variants_[j], variants_[j - 1]);
        // Duplicate max_values make selection order-dependent (load defaults to the
        // last, set_size() picks the first with max_value >= size), so the same
        // requested size would resolve to different models. Reject the ambiguity.
        for (std::size_t i = 1; i < variants_.size(); ++i)
            if (variants_[i].max_value == variants_[i - 1].max_value)
                return fail("A2: duplicate submodel max_value is ambiguous");
        active_ = static_cast<int>(variants_.size()) - 1;
        return true;
    }

    void reset() { for (Variant& v : variants_) v.model.reset(); }
    void prewarm() { for (Variant& v : variants_) v.model.prewarm(); }
    float process_sample(float x) { return variants_[static_cast<std::size_t>(active_)].model.process_sample(x); }
    void process(const float* in, float* out, std::uint32_t n) {
        variants_[static_cast<std::size_t>(active_)].model.process(in, out, n);
    }

    bool ok() const { return !variants_.empty(); }
    double sample_rate() const { return sr_; }
    int channels() const {
        return variants_.empty() ? 0 : variants_[static_cast<std::size_t>(active_)].model.channels();
    }
    int variant_count() const { return static_cast<int>(variants_.size()); }
    int active_variant() const { return active_; }

    // Select a size in [0,1]: the smallest variant whose max_value >= size, else
    // the largest. Returns true iff the active variant changed.
    bool set_size(double size) {
        int pick = static_cast<int>(variants_.size()) - 1;
        for (std::size_t i = 0; i < variants_.size(); ++i)
            if (variants_[i].max_value >= size) { pick = static_cast<int>(i); break; }
        const bool changed = pick != active_;
        active_ = pick;
        return changed;
    }

private:
    struct Variant { double max_value = 1.0; A2Model model; };

    bool add_variant(const choc::value::ValueView& model, double max_value, std::string* error) {
        A2ArrayConfig arr;
        std::vector<float> weights;
        double sr = sr_;
        if (!detail_a2::parse_submodel(model, arr, weights, sr, error)) return false;
        Variant v;
        v.max_value = max_value;
        if (!v.model.build(std::move(arr), std::move(weights), sr, error)) return false;
        if (sr_ <= 0.0) sr_ = sr;
        variants_.push_back(std::move(v));
        return true;
    }

    std::vector<Variant> variants_;
    int active_ = 0;
    double sr_ = -1.0;
};

// Load an A2 (.nam) — SlimmableContainer or A2-shaped WaveNet — from a file.
inline bool load_nam_a2(const std::string& path, NamA2& out, std::string* error = nullptr) {
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
    return out.build(root, error);
}

// Classify a parsed root as A2 (SlimmableContainer, or an A2-shaped WaveNet). Lets
// the runtime route WaveNet-A2 away from the A1 loader, which shares the string.
inline bool is_nam_a2(const choc::value::ValueView& root) {
    if (!root.isObject()) return false;
    const std::string arch =
        root.hasObjectMember("architecture") ? std::string(root["architecture"].getString()) : std::string();
    if (arch == "SlimmableContainer") return true;
    if (arch == "WaveNet" && root.hasObjectMember("config"))
        return detail_a2::looks_like_a2(root["config"]);
    return false;
}

}  // namespace pulp::examples::nam
