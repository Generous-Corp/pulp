#pragma once

// Exact, reference-faithful CPU inference for the open-source Neural Amp Modeler
// (NAM) "WaveNet" architecture, plus a ``.nam`` file loader. This is the oracle
// (and always-available fallback) the GPU primitive is validated against — so the
// math and the weight-consumption order match NAM's serialization byte-for-byte.
//
// A ``.nam`` file is JSON:
//   { version, metadata, architecture:"WaveNet",
//     config:{ layers:[ ...layer-array configs... ], head, head_scale },
//     weights:[ flat f32 ... ], sample_rate }
//
// The flat ``weights`` array is consumed in this exact order (matching NAM's
// LayerArray/Layer/Conv1D/Conv1x1 ``set_weights_``):
//
//   for each layer array:
//     rechannel        Conv1x1  in=input_size      out=channels        no bias
//     for each dilated layer:
//       conv           Conv1D   in=channels        out=Z   kernel=K dilation=d  bias
//       input_mixin    Conv1x1  in=condition_size  out=Z               no bias
//       layer1x1       Conv1x1  in=channels        out=channels        bias
//     head_rechannel   Conv1x1  in=channels        out=head_size       bias=head_bias
//   head_scale         one scalar (the final weight)
//
// where Z = gated ? 2*channels : channels.
//
// Within a Conv1D/Conv1x1 the layout is, with groups == 1:
//   weights[out][in][k]   (for each out channel, each in channel, each kernel tap)
//   then bias[out]        (if the module has a bias)
// The dilated conv is causal: kernel tap k reads the input sample (K-1-k)*dilation
// in the past, so tap K-1 is the current sample. A Conv1x1 is just this with
// kernel=1, dilation=1 (a per-sample linear map y = W x + b).
//
// Conditioning: the WaveNet's condition signal is the raw input. Layer-array 0
// takes the input as both its layer input and its condition; subsequent arrays
// take the previous array's layer output as their layer input but still use the
// raw input as the condition. The per-array head accumulator is chained: array
// i>0 seeds its accumulator with array i-1's (re-channeled) head output, adds
// each of its own layers' skip outputs, then re-channels to head_size. The last
// array's head output, times head_scale, is the model output.
//
// Activation default is "Tanh" => std::tanh (NAM's fast-tanh is an opt-in
// approximation, off by default; this reference uses the exact std::tanh path).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::nam {

// One dilated, causal 1-D convolution. With kernel=1, dilation=1 this is exactly
// a NAM Conv1x1 (a per-sample linear layer), so every NAM module maps onto it.
// Stateful: carries the input history across blocks via a ring buffer, so step()
// can be called sample-by-sample like a real-time engine.
class Conv {
public:
    void configure(int in_ch, int out_ch, int kernel, int dilation, bool bias) {
        in_ch_ = in_ch;
        out_ch_ = out_ch;
        kernel_ = kernel;
        dilation_ = dilation;
        has_bias_ = bias;
        weight_.assign(static_cast<std::size_t>(kernel), std::vector<float>(
            static_cast<std::size_t>(out_ch) * static_cast<std::size_t>(in_ch), 0.0f));
        bias_.assign(bias ? static_cast<std::size_t>(out_ch) : 0u, 0.0f);
        reach_ = (kernel - 1) * dilation;            // oldest tap reaches this far back
        slots_ = reach_ + 1;                          // history columns (incl. current)
        reset();
    }

    // Number of flat floats this module consumes from the weights array.
    std::size_t weight_count() const {
        return static_cast<std::size_t>(out_ch_) * static_cast<std::size_t>(in_ch_)
                   * static_cast<std::size_t>(kernel_)
               + (has_bias_ ? static_cast<std::size_t>(out_ch_) : 0u);
    }

    // Consume weights in NAM's exact order: [out][in][k], then bias[out].
    void load(const float*& p) {
        for (int i = 0; i < out_ch_; ++i)
            for (int j = 0; j < in_ch_; ++j)
                for (int k = 0; k < kernel_; ++k)
                    weight_[static_cast<std::size_t>(k)][static_cast<std::size_t>(i) * in_ch_ + j] = *p++;
        if (has_bias_)
            for (int i = 0; i < out_ch_; ++i)
                bias_[static_cast<std::size_t>(i)] = *p++;
    }

    void reset() {
        history_.assign(static_cast<std::size_t>(slots_) * in_ch_, 0.0f);
        head_ = 0;
    }

    // One sample in (in_ch values) -> one sample out (out_ch values).
    void step(const float* x, float* y) {
        // Write the current input vector into the ring at head_.
        float* cur_slot = &history_[static_cast<std::size_t>(head_) * in_ch_];
        for (int j = 0; j < in_ch_; ++j) cur_slot[j] = x[j];

        for (int i = 0; i < out_ch_; ++i) y[i] = has_bias_ ? bias_[static_cast<std::size_t>(i)] : 0.0f;

        for (int k = 0; k < kernel_; ++k) {
            const int back = (kernel_ - 1 - k) * dilation_;   // samples into the past
            int idx = head_ - back;
            idx %= slots_;
            if (idx < 0) idx += slots_;
            const float* past = &history_[static_cast<std::size_t>(idx) * in_ch_];
            const std::vector<float>& wk = weight_[static_cast<std::size_t>(k)];
            for (int i = 0; i < out_ch_; ++i) {
                const float* wrow = &wk[static_cast<std::size_t>(i) * in_ch_];
                float acc = 0.0f;
                for (int j = 0; j < in_ch_; ++j) acc += wrow[j] * past[j];
                y[i] += acc;
            }
        }
        head_ += 1;
        if (head_ >= slots_) head_ = 0;
    }

    int in_channels() const { return in_ch_; }
    int out_channels() const { return out_ch_; }

private:
    int in_ch_ = 0, out_ch_ = 0, kernel_ = 1, dilation_ = 1;
    bool has_bias_ = false;
    int reach_ = 0, slots_ = 1, head_ = 0;
    std::vector<std::vector<float>> weight_;   // weight_[k][out*in_ch + in]
    std::vector<float> bias_;
    std::vector<float> history_;               // ring buffer: slots_ x in_ch_
};

enum class Activation { Tanh, Hardtanh, Fasttanh, ReLU, Sigmoid, Identity };

inline Activation parse_activation(const std::string& name) {
    if (name == "Tanh") return Activation::Tanh;
    if (name == "Hardtanh") return Activation::Hardtanh;
    if (name == "Fasttanh") return Activation::Fasttanh;
    if (name == "ReLU") return Activation::ReLU;
    if (name == "Sigmoid") return Activation::Sigmoid;
    return Activation::Tanh;   // NAM's default activation
}

inline float apply_activation(Activation a, float x) {
    switch (a) {
        case Activation::Tanh: return std::tanh(x);
        case Activation::Hardtanh: return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
        case Activation::Fasttanh: return std::tanh(x);   // exact path; the approximation is an opt-in optimization

        case Activation::ReLU: return x > 0.0f ? x : 0.0f;
        case Activation::Sigmoid: return 1.0f / (1.0f + std::exp(-x));
        case Activation::Identity: return x;
    }
    return std::tanh(x);
}

// Static per-layer-array configuration parsed from the .nam JSON.
struct LayerArrayConfig {
    int input_size = 1;
    int condition_size = 1;
    int channels = 1;
    int kernel_size = 1;
    std::vector<int> dilations;
    int head_size = 1;
    std::string activation = "Tanh";
    bool gated = false;
    bool head_bias = false;
};

// A single WaveNet layer: dilated conv + input-mixin + activation (optionally
// gated) + 1x1 residual, producing a residual output (to the next layer) and a
// skip/head output (accumulated by the layer array).
class Layer {
public:
    void configure(const LayerArrayConfig& cfg, int dilation) {
        channels_ = cfg.channels;
        condition_size_ = cfg.condition_size;
        gated_ = cfg.gated;
        activation_ = parse_activation(cfg.activation);
        const int z = gated_ ? 2 * channels_ : channels_;
        conv_.configure(channels_, z, cfg.kernel_size, dilation, /*bias=*/true);
        input_mixin_.configure(condition_size_, z, /*kernel=*/1, /*dilation=*/1, /*bias=*/false);
        layer1x1_.configure(channels_, channels_, /*kernel=*/1, /*dilation=*/1, /*bias=*/true);
        z_.assign(static_cast<std::size_t>(z), 0.0f);
        mix_.assign(static_cast<std::size_t>(z), 0.0f);
        l1_.assign(static_cast<std::size_t>(channels_), 0.0f);
    }

    std::size_t weight_count() const {
        return conv_.weight_count() + input_mixin_.weight_count() + layer1x1_.weight_count();
    }

    void load(const float*& p) {
        conv_.load(p);
        input_mixin_.load(p);
        layer1x1_.load(p);
    }

    void reset() {
        conv_.reset();
        input_mixin_.reset();
        layer1x1_.reset();
    }

    // input: channels_, condition: condition_size_,
    // out_next: channels_ (residual), out_head: channels_ (skip/head).
    void step(const float* input, const float* condition, float* out_next, float* out_head) {
        conv_.step(input, z_.data());
        input_mixin_.step(condition, mix_.data());
        const int z = static_cast<int>(z_.size());
        for (int i = 0; i < z; ++i) z_[static_cast<std::size_t>(i)] += mix_[static_cast<std::size_t>(i)];

        if (!gated_) {
            for (int c = 0; c < channels_; ++c)
                z_[static_cast<std::size_t>(c)] = apply_activation(activation_, z_[static_cast<std::size_t>(c)]);
            for (int c = 0; c < channels_; ++c) out_head[c] = z_[static_cast<std::size_t>(c)];
            layer1x1_.step(z_.data(), l1_.data());
        } else {
            // Top half: primary activation. Bottom half: sigmoid gate. Product -> head.
            for (int c = 0; c < channels_; ++c) {
                const float a = apply_activation(activation_, z_[static_cast<std::size_t>(c)]);
                const float g = apply_activation(Activation::Sigmoid, z_[static_cast<std::size_t>(channels_ + c)]);
                out_head[c] = a * g;
            }
            layer1x1_.step(out_head, l1_.data());
        }
        for (int c = 0; c < channels_; ++c) out_next[c] = input[c] + l1_[static_cast<std::size_t>(c)];
    }

    int channels() const { return channels_; }

private:
    int channels_ = 1, condition_size_ = 1;
    bool gated_ = false;
    Activation activation_ = Activation::Tanh;
    Conv conv_, input_mixin_, layer1x1_;
    std::vector<float> z_, mix_, l1_;
};

// A chain of layers sharing channels/kernel/activation. Input is re-channeled to
// the layer channel count; each layer's skip output is accumulated; the running
// accumulator is re-channeled to head_size.
class LayerArray {
public:
    void configure(const LayerArrayConfig& cfg) {
        cfg_ = cfg;
        rechannel_.configure(cfg.input_size, cfg.channels, /*kernel=*/1, /*dilation=*/1, /*bias=*/false);
        layers_.clear();
        layers_.resize(cfg.dilations.size());
        for (std::size_t i = 0; i < cfg.dilations.size(); ++i)
            layers_[i].configure(cfg, cfg.dilations[i]);
        // head_output_size == bottleneck == channels (no head1x1 in the standard model).
        head_rechannel_.configure(cfg.channels, cfg.head_size, /*kernel=*/1, /*dilation=*/1, /*bias=*/cfg.head_bias);
        rc_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
        acc_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
        cur_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
        next_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
        skip_.assign(static_cast<std::size_t>(cfg.channels), 0.0f);
    }

    std::size_t weight_count() const {
        std::size_t n = rechannel_.weight_count();
        for (const Layer& l : layers_) n += l.weight_count();
        n += head_rechannel_.weight_count();
        return n;
    }

    void load(const float*& p) {
        rechannel_.load(p);
        for (Layer& l : layers_) l.load(p);
        head_rechannel_.load(p);
    }

    void reset() {
        rechannel_.reset();
        for (Layer& l : layers_) l.reset();
        head_rechannel_.reset();
    }

    // layer_inputs: input_size, condition: condition_size,
    // head_in: channels_ (previous array's head output) or nullptr for the first array,
    // layer_out: channels_, head_out: head_size.
    void step(const float* layer_inputs, const float* condition, const float* head_in,
              float* layer_out, float* head_out) {
        rechannel_.step(layer_inputs, rc_.data());
        const int C = cfg_.channels;
        for (int i = 0; i < C; ++i) acc_[static_cast<std::size_t>(i)] = head_in ? head_in[i] : 0.0f;
        for (int i = 0; i < C; ++i) cur_[static_cast<std::size_t>(i)] = rc_[static_cast<std::size_t>(i)];

        for (std::size_t l = 0; l < layers_.size(); ++l) {
            layers_[l].step(cur_.data(), condition, next_.data(), skip_.data());
            for (int i = 0; i < C; ++i) acc_[static_cast<std::size_t>(i)] += skip_[static_cast<std::size_t>(i)];
            for (int i = 0; i < C; ++i) cur_[static_cast<std::size_t>(i)] = next_[static_cast<std::size_t>(i)];
        }
        for (int i = 0; i < C; ++i) layer_out[i] = cur_[static_cast<std::size_t>(i)];
        head_rechannel_.step(acc_.data(), head_out);
    }

    int channels() const { return cfg_.channels; }
    int head_size() const { return cfg_.head_size; }

private:
    LayerArrayConfig cfg_;
    Conv rechannel_;
    std::vector<Layer> layers_;
    Conv head_rechannel_;
    std::vector<float> rc_, acc_, cur_, next_, skip_;
};

// The full NAM WaveNet model: configuration, weights, and stateful inference.
class NamModel {
public:
    // Parse + build from already-parsed config + weights. Returns false (and sets
    // error()) if the architecture is unsupported or the weight count is wrong.
    bool build(std::vector<LayerArrayConfig> arrays, float head_scale_config,
               std::vector<float> weights, int in_channels, double sample_rate) {
        arrays_cfg_ = std::move(arrays);
        head_scale_config_ = head_scale_config;
        weights_ = std::move(weights);
        in_channels_ = in_channels;
        sample_rate_ = sample_rate;
        error_.clear();

        if (arrays_cfg_.empty()) { error_ = "WaveNet requires at least one layer array"; return false; }

        // Shape / memory-safety guards. The per-sample forward feeds array 0 a
        // 1-wide input (the mono sample) and chains each later array from the
        // previous array's channel count. A file whose shapes don't line up would
        // read past the fixed-size input buffers, so reject it up front.
        if (in_channels_ != 1) {
            error_ = "only mono input (in_channels == 1) is supported";
            return false;
        }
        if (arrays_cfg_[0].input_size != in_channels_) {
            error_ = "first layer array input_size must equal in_channels (1)";
            return false;
        }
        for (std::size_t i = 1; i < arrays_cfg_.size(); ++i)
            if (arrays_cfg_[i].input_size != arrays_cfg_[i - 1].channels) {
                error_ = "layer array " + std::to_string(i)
                         + " input_size must equal the previous array's channel count";
                return false;
            }

        arrays_.clear();
        arrays_.resize(arrays_cfg_.size());
        for (std::size_t i = 0; i < arrays_cfg_.size(); ++i) arrays_[i].configure(arrays_cfg_[i]);

        // Expected = sum of array weight counts + 1 (the trailing head_scale).
        std::size_t expected = 1;
        for (const LayerArray& a : arrays_) expected += a.weight_count();
        expected_weight_count_ = expected;

        if (weights_.size() != expected) {
            std::ostringstream ss;
            ss << "weight count mismatch: file provides " << weights_.size()
               << " floats, model layout consumes " << expected;
            error_ = ss.str();
            return false;
        }

        const float* p = weights_.data();
        for (LayerArray& a : arrays_) a.load(p);
        head_scale_ = *p++;                            // NAM stores head_scale as the final weight.
        weights_consumed_ = static_cast<std::size_t>(p - weights_.data());

        out_channels_ = arrays_.back().head_size();
        head_buf_a_.assign(64, 0.0f);
        head_buf_b_.assign(64, 0.0f);
        layer_buf_a_.assign(64, 0.0f);
        layer_buf_b_.assign(64, 0.0f);
        return weights_consumed_ == weights_.size();
    }

    void reset() {
        for (LayerArray& a : arrays_) a.reset();
    }

    // Samples of past input that influence the current output — the sum over every
    // dilated layer of its causal reach (kernel-1)*dilation. Used to size a prewarm
    // long enough to fully populate the dilation history.
    int receptive_field() const {
        int rf = 0;
        for (const LayerArrayConfig& a : arrays_cfg_)
            for (int d : a.dilations) rf += (a.kernel_size - 1) * d;
        return rf;
    }

    // Settle the model at its silence steady-state: reset, then run enough silence
    // to fully fill the dilation history. A NAM capture's response to silence is
    // NOT zero (biases), and the dilated buffers take a full receptive field to
    // populate — so a cold start (reset only) produces a ~receptive-field-long DC
    // transient and does not match the reference, which prewarms on load. Call this
    // instead of reset() before the model goes live on the audio path. Off-thread
    // (it runs thousands of samples); never the audio thread.
    void prewarm() {
        reset();
        // Margin past a full receptive field, clamped so a pathological model
        // (huge dilations) can't turn prewarm into a multi-second stall. Real
        // captures settle in a few thousand samples; the cap is far above that.
        constexpr long long kMaxPrewarm = 1 << 18;   // 262144 samples
        long long n = 2LL * receptive_field() + 512;
        if (n < 0) n = 512;
        if (n > kMaxPrewarm) n = kMaxPrewarm;
        for (long long i = 0; i < n; ++i) process_sample(0.0f);
    }

    // One mono sample in -> one mono output sample. The per-sample core; process()
    // wraps it for a block. Carries dilation history across calls (streaming).
    float process_sample(float x) {
        const float cond[1] = {x};                      // condition == raw input
        float in0[1] = {x};                             // array-0 layer input == raw input

        const float* layer_in = in0;
        int layer_in_size = in_channels_;
        const float* head_in = nullptr;

        std::vector<float>* lo_cur = &layer_buf_a_;
        std::vector<float>* lo_prev = &layer_buf_b_;
        std::vector<float>* ho_cur = &head_buf_a_;
        std::vector<float>* ho_prev = &head_buf_b_;

        for (std::size_t i = 0; i < arrays_.size(); ++i) {
            const int C = arrays_[i].channels();
            const int H = arrays_[i].head_size();
            if (static_cast<int>(lo_cur->size()) < C) lo_cur->resize(static_cast<std::size_t>(C));
            if (static_cast<int>(ho_cur->size()) < H) ho_cur->resize(static_cast<std::size_t>(H));
            (void)layer_in_size;
            arrays_[i].step(layer_in, cond, head_in, lo_cur->data(), ho_cur->data());

            // Next array consumes this array's layer output + (chained) head output.
            layer_in = lo_cur->data();
            layer_in_size = C;
            head_in = ho_cur->data();
            std::swap(lo_cur, lo_prev);
            std::swap(ho_cur, ho_prev);
        }
        // After the loop the last array's head output lives in ho_prev (post-swap).
        return head_scale_ * (*ho_prev)[0];
    }

    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) out[i] = process_sample(in[i]);
    }

    // Accessors / diagnostics.
    const std::vector<LayerArrayConfig>& arrays() const { return arrays_cfg_; }
    float head_scale() const { return head_scale_; }
    float head_scale_config() const { return head_scale_config_; }
    int in_channels() const { return in_channels_; }
    int out_channels() const { return out_channels_; }
    double sample_rate() const { return sample_rate_; }
    std::size_t weights_size() const { return weights_.size(); }
    // Read-only view of the flat weight blob (in NAM serialization order,
    // including the trailing head_scale scalar). Lets a GPU adapter upload the
    // exact same weights the CPU oracle runs, so both forwards stay identical.
    const float* weights_data() const { return weights_.data(); }
    std::size_t weights_consumed() const { return weights_consumed_; }
    std::size_t expected_weight_count() const { return expected_weight_count_; }
    const std::string& error() const { return error_; }

private:
    std::vector<LayerArrayConfig> arrays_cfg_;
    std::vector<LayerArray> arrays_;
    std::vector<float> weights_;
    float head_scale_ = 1.0f;
    float head_scale_config_ = 1.0f;
    int in_channels_ = 1;
    int out_channels_ = 1;
    double sample_rate_ = -1.0;
    std::size_t weights_consumed_ = 0;
    std::size_t expected_weight_count_ = 0;
    std::string error_;

    // Ping-pong scratch for inter-array layer/head outputs (resized on demand).
    std::vector<float> layer_buf_a_, layer_buf_b_, head_buf_a_, head_buf_b_;
};

// ---------------------------------------------------------------------------
// .nam JSON loader (choc::json).
// ---------------------------------------------------------------------------

namespace detail {

inline double num(const choc::value::ValueView& v) {
    if (v.isInt64()) return static_cast<double>(v.getInt64());
    if (v.isInt32()) return static_cast<double>(v.getInt32());
    if (v.isFloat64()) return v.getFloat64();
    if (v.isFloat32()) return static_cast<double>(v.getFloat32());
    if (v.isBool()) return v.getBool() ? 1.0 : 0.0;
    return v.getWithDefault<double>(0.0);
}

} // namespace detail

// Load a .nam file at ``path`` into ``out``. Returns false and sets out.error()
// (or the local ``error`` string when the file/JSON can't even be opened) on
// failure. Only the standard WaveNet (non-FiLM, layer1x1 active, head1x1
// inactive, groups==1) is supported — the shape of all published .nam captures.
inline bool load_nam(const std::string& path, NamModel& out, std::string* error = nullptr) {
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

    const std::string architecture =
        root.hasObjectMember("architecture") ? std::string(root["architecture"].getString()) : std::string();
    if (architecture != "WaveNet")
        return fail("unsupported architecture: '" + architecture + "' (only WaveNet)");

    if (!root.hasObjectMember("config")) return fail("missing 'config'");
    const choc::value::ValueView config = root["config"];

    const double sample_rate =
        root.hasObjectMember("sample_rate") ? detail::num(root["sample_rate"]) : -1.0;
    const int in_channels =
        config.hasObjectMember("in_channels") ? static_cast<int>(detail::num(config["in_channels"])) : 1;

    if (config.hasObjectMember("head") && !config["head"].isVoid()) {
        // The post-stack head is unused by published captures and is not modeled here.
        // (NAM itself throws "Head not implemented!" for this path.)
        return fail("config.head is set; post-stack head is not supported");
    }

    if (!config.hasObjectMember("layers")) return fail("missing 'config.layers'");
    const choc::value::ValueView layers = config["layers"];
    if (!layers.isArray() || layers.size() == 0) return fail("'config.layers' must be a non-empty array");

    std::vector<LayerArrayConfig> arrays;
    arrays.reserve(layers.size());
    for (uint32_t i = 0; i < layers.size(); ++i) {
        const choc::value::ValueView L = layers[i];
        LayerArrayConfig a;
        a.input_size = static_cast<int>(detail::num(L["input_size"]));
        a.condition_size = static_cast<int>(detail::num(L["condition_size"]));
        a.channels = static_cast<int>(detail::num(L["channels"]));
        a.kernel_size = static_cast<int>(detail::num(L["kernel_size"]));
        a.head_size = static_cast<int>(detail::num(L["head_size"]));
        a.head_bias = L.hasObjectMember("head_bias") && L["head_bias"].getWithDefault<bool>(false);
        a.gated = L.hasObjectMember("gated") && L["gated"].getWithDefault<bool>(false);
        if (L.hasObjectMember("activation") && L["activation"].isString())
            a.activation = std::string(L["activation"].getString());

        // Reject the advanced knobs this reference does not model, so we never
        // silently produce wrong audio for a non-standard capture.
        auto active = [&](const char* key) {
            return L.hasObjectMember(key) && L[key].isObject() && L[key].hasObjectMember("active")
                   && L[key]["active"].getWithDefault<bool>(false);
        };
        if (L.hasObjectMember("bottleneck")
            && static_cast<int>(detail::num(L["bottleneck"])) != a.channels)
            return fail("layer " + std::to_string(i) + ": bottleneck != channels is not supported");
        // The forward feeds every layer a 1-dimensional condition (the raw input).
        // A condition_size > 1 is a parametric/conditioned model whose extra
        // condition channels come from host controls we do not wire — running it
        // would read past the 1-element condition buffer and mis-render. Reject it.
        if (a.condition_size != 1)
            return fail("layer " + std::to_string(i)
                        + ": condition_size != 1 (parametric/conditioned model) not supported");
        if (active("head1x1")) return fail("layer " + std::to_string(i) + ": head1x1 not supported");
        if (L.hasObjectMember("layer1x1") && L["layer1x1"].isObject()
            && L["layer1x1"].hasObjectMember("active")
            && !L["layer1x1"]["active"].getWithDefault<bool>(true))
            return fail("layer " + std::to_string(i) + ": inactive layer1x1 not supported");
        for (const char* k : {"groups_input", "groups_input_mixin"})
            if (L.hasObjectMember(k) && static_cast<int>(detail::num(L[k])) != 1)
                return fail("layer " + std::to_string(i) + ": grouped conv not supported");

        const choc::value::ValueView dil = L["dilations"];
        if (!dil.isArray() || dil.size() == 0)
            return fail("layer " + std::to_string(i) + ": 'dilations' must be a non-empty array");
        for (uint32_t d = 0; d < dil.size(); ++d) {
            const int dv = static_cast<int>(detail::num(dil[d]));
            // Keep the receptive field (and hence prewarm cost) bounded and int-safe.
            // Real captures top out around 512; the cap is far above that.
            if (dv <= 0 || dv > (1 << 16))
                return fail("layer " + std::to_string(i) + ": dilation out of range (1.." + std::to_string(1 << 16) + ")");
            a.dilations.push_back(dv);
        }
        arrays.push_back(std::move(a));
    }

    const float head_scale =
        config.hasObjectMember("head_scale") ? static_cast<float>(detail::num(config["head_scale"])) : 1.0f;

    if (!root.hasObjectMember("weights")) return fail("missing 'weights'");
    const choc::value::ValueView wv = root["weights"];
    if (!wv.isArray()) return fail("'weights' must be an array");
    std::vector<float> weights;
    weights.reserve(wv.size());
    for (uint32_t i = 0; i < wv.size(); ++i) weights.push_back(static_cast<float>(detail::num(wv[i])));

    const bool ok = out.build(std::move(arrays), head_scale, std::move(weights), in_channels, sample_rate);
    if (!ok && error) *error = out.error();
    return ok;
}

} // namespace pulp::examples::nam
