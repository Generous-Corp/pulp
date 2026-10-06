#pragma once

// Private CPU bridge for the serialized NAM WaveNet/A1 format.  This is an
// independent implementation of the open format; it deliberately does not
// include or link the GPU-NAM tree.  Parsing and all storage preparation happen
// off the callback.  The prepared object only mutates fixed-size causal state.

#include "nam_tcn_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <choc/text/choc_JSON.h>

namespace pulp::gpu_audio::detail {

class NamTcnArtifact final {
  public:
    NamTcnArtifact() = default;
    NamTcnArtifact(const NamTcnArtifact&) = delete;
    NamTcnArtifact& operator=(const NamTcnArtifact&) = delete;
    NamTcnArtifact(NamTcnArtifact&&) noexcept = default;
    NamTcnArtifact& operator=(NamTcnArtifact&&) noexcept = default;

    bool load(const std::string& path, std::string* error = nullptr) {
        auto fail = [&](std::string message) {
            if (error != nullptr)
                *error = std::move(message);
            return false;
        };
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return fail("could not open artifact: " + path);
        std::ostringstream contents;
        contents << file.rdbuf();
        if (contents.str().empty())
            return fail("artifact is empty: " + path);

        choc::value::Value root;
        try {
            root = choc::json::parse(contents.str());
        } catch (const std::exception& exception) {
            return fail(std::string("JSON parse error: ") + exception.what());
        }
        if (!root.isObject())
            return fail("artifact root is not an object");
        if (!root.hasObjectMember("architecture") || !root["architecture"].isString() ||
            std::string(root["architecture"].getString()) != "WaveNet")
            return fail("only the serialized WaveNet A1 architecture is supported");
        if (!root.hasObjectMember("config") || !root["config"].isObject())
            return fail("missing object config");
        const auto config = root["config"];
        if (config.hasObjectMember("head") && !config["head"].isVoid())
            return fail("post-stack head is unsupported");
        if (!config.hasObjectMember("layers") || !config["layers"].isArray() ||
            config["layers"].size() == 0 || config["layers"].size() > kMaxArrays)
            return fail("config.layers must be a bounded non-empty array");

        std::vector<ArrayConfig> parsed;
        parsed.reserve(config["layers"].size());
        for (std::uint32_t index = 0; index < config["layers"].size(); ++index) {
            const auto layer = config["layers"][index];
            if (!layer.isObject())
                return fail("layer entry is not an object");
            if ((layer.hasObjectMember("kernel_sizes") && layer["kernel_sizes"].isArray()) ||
                (layer.hasObjectMember("head") && layer["head"].isObject()) ||
                (layer.hasObjectMember("activation") && layer["activation"].isArray()))
                return fail("A2-shaped layer is unsupported by the A1 bridge");
            if (layer.hasObjectMember("state_offset") || layer.hasObjectMember("state_offsets"))
                return fail("explicit state offsets are unsupported");

            bool ok = true;
            ArrayConfig current;
            current.input_size = integer(layer["input_size"], 1, kMaxChannels, ok);
            current.condition_size = integer(layer["condition_size"], 1, 1, ok);
            current.channels = integer(layer["channels"], 1, kMaxChannels, ok);
            current.kernel_size = integer(layer["kernel_size"], 1, kMaxKernel, ok);
            current.head_size = integer(layer["head_size"], 1, kMaxChannels, ok);
            current.gated =
                layer.hasObjectMember("gated") && layer["gated"].getWithDefault<bool>(false);
            current.head_bias = layer.hasObjectMember("head_bias") &&
                                layer["head_bias"].getWithDefault<bool>(false);
            if (!ok)
                return fail("invalid layer shape integer");
            if (layer.hasObjectMember("activation")) {
                if (!layer["activation"].isString())
                    return fail("activation must be a string");
                current.activation = std::string(layer["activation"].getString());
            }
            if (current.condition_size != 1)
                return fail("condition_size != 1 is unsupported");
            if (layer.hasObjectMember("bottleneck") &&
                integer(layer["bottleneck"], 1, kMaxChannels, ok) != current.channels)
                return fail("bottleneck != channels is unsupported");
            if (active(layer, "head1x1"))
                return fail("head1x1 is unsupported");
            if (layer.hasObjectMember("layer1x1") && layer["layer1x1"].isObject() &&
                layer["layer1x1"].hasObjectMember("active") &&
                !layer["layer1x1"]["active"].getWithDefault<bool>(true))
                return fail("inactive layer1x1 is unsupported");
            for (const char* key : {"groups_input", "groups_input_mixin"})
                if (layer.hasObjectMember(key) && integer(layer[key], 1, 1, ok) != 1)
                    return fail("grouped convolution is unsupported");
            if (!ok || !layer.hasObjectMember("dilations") || !layer["dilations"].isArray() ||
                layer["dilations"].size() == 0 || layer["dilations"].size() > kMaxLayers)
                return fail("invalid dilations");
            current.dilations.reserve(layer["dilations"].size());
            for (std::uint32_t d = 0; d < layer["dilations"].size(); ++d) {
                const int dilation = integer(layer["dilations"][d], 1, kMaxDilation, ok);
                if (!ok)
                    return fail("invalid dilation");
                current.dilations.push_back(dilation);
            }
            parsed.push_back(std::move(current));
        }
        if (parsed.front().input_size != 1)
            return fail("first layer input_size must be one");
        for (std::size_t i = 1; i < parsed.size(); ++i)
            if (parsed[i].input_size != parsed[i - 1].channels)
                return fail("layer input_size does not match previous channels");

        if (!root.hasObjectMember("weights") || !root["weights"].isArray() ||
            root["weights"].size() > kMaxWeights)
            return fail("missing or oversized weights array");
        std::vector<float> weights;
        weights.reserve(root["weights"].size());
        for (std::uint32_t i = 0; i < root["weights"].size(); ++i) {
            const double value = number(root["weights"][i]);
            if (!std::isfinite(value) || value > std::numeric_limits<float>::max() ||
                value < std::numeric_limits<float>::lowest())
                return fail("non-finite or out-of-range weight");
            weights.push_back(static_cast<float>(value));
        }
        bool ok = true;
        const float scale = root["config"].hasObjectMember("head_scale")
                                ? finite_float(number(root["config"]["head_scale"]), ok)
                                : 1.0f;
        if (!ok)
            return fail("invalid head_scale");

        Model candidate;
        if (!candidate.configure(parsed, weights, scale, error))
            return false;
        const auto sample_rate = root.hasObjectMember("sample_rate")
                                    ? number(root["sample_rate"])
                                    : -1.0;
        if (!(sample_rate > 0.0) || !std::isfinite(sample_rate))
            return fail("invalid sample_rate");
        // Commit the fully validated candidate only after every field has
        // passed validation. A failed replacement must leave the live
        // artifact, including its metadata and causal state, untouched.
        arrays_ = std::move(candidate.arrays);
        head_scale_ = candidate.head_scale;
        sample_rate_ = sample_rate;
        receptive_field_ = candidate.receptive_field;
        state_bytes_ = candidate.state_bytes;
        weights_size_ = weights.size();
        loaded_ = true;
        reset();
        return true;
    }

    bool loaded() const noexcept {
        return loaded_;
    }
    std::uint32_t receptive_field() const noexcept {
        return receptive_field_;
    }
    std::uint64_t state_bytes() const noexcept {
        return state_bytes_;
    }
    double sample_rate() const noexcept {
        return sample_rate_;
    }
    std::size_t weights_size() const noexcept {
        return weights_size_;
    }

    void reset() noexcept {
        for (auto& array : arrays_)
            array.reset();
    }

    void process(const float* input, float* output, std::uint32_t frames) noexcept {
        for (std::uint32_t i = 0; i < frames; ++i)
            output[i] = process_sample(input[i]);
    }

  private:
    static constexpr int kMaxArrays = 32, kMaxLayers = 64, kMaxChannels = 64, kMaxKernel = 64,
                         kMaxDilation = 1 << 16;
    static constexpr std::size_t kMaxWeights = 8u * 1024u * 1024u;

    struct ArrayConfig {
        int input_size = 1, condition_size = 1, channels = 1, kernel_size = 1, head_size = 1;
        bool gated = false, head_bias = false;
        std::string activation = "Tanh";
        std::vector<int> dilations;
    };
    struct Conv {
        int in = 0, out = 0, kernel = 1, dilation = 1, slots = 1, head = 0;
        bool bias = false;
        std::vector<float> weights, biases, history;
        std::size_t count() const noexcept {
            return static_cast<std::size_t>(in) * out * kernel + (bias ? out : 0);
        }
        bool configure(int input, int output, int k, int d, bool with_bias) {
            in = input;
            out = output;
            kernel = k;
            dilation = d;
            bias = with_bias;
            const auto reach = (kernel - 1) * dilation;
            slots = reach + 1;
            head = 0;
            weights.assign(static_cast<std::size_t>(in) * out * kernel, 0.0f);
            biases.assign(with_bias ? static_cast<std::size_t>(out) : 0, 0.0f);
            history.assign(static_cast<std::size_t>(slots) * in, 0.0f);
            return true;
        }
        void load(const float*& p) noexcept {
            // NAM serializes Conv weights as [out][in][kernel], while the
            // callback layout indexes [kernel][out][in]. Transpose once while
            // preparing so the callback performs only contiguous row walks.
            for (int o = 0; o < out; ++o)
                for (int i = 0; i < in; ++i)
                    for (int k = 0; k < kernel; ++k)
                        weights[(static_cast<std::size_t>(k) * out + o) * in + i] = *p++;
            if (bias) {
                std::copy(p, p + biases.size(), biases.begin());
                p += biases.size();
            }
        }
        void reset() noexcept {
            std::fill(history.begin(), history.end(), 0.0f);
            head = 0;
        }
        void step(const float* x, float* y) noexcept {
            for (int o = 0; o < out; ++o)
                y[o] = bias ? biases[static_cast<std::size_t>(o)] : 0.0f;
            if (kernel == 1) {
                for (int o = 0; o < out; ++o) {
                    float acc = 0.0f;
                    for (int i = 0; i < in; ++i)
                        acc += weights[static_cast<std::size_t>(o) * in + i] * x[i];
                    y[o] += acc;
                }
                return;
            }
            float* current = history.data() + static_cast<std::size_t>(head) * in;
            std::copy(x, x + in, current);
            for (int k = 0; k < kernel; ++k) {
                int slot = head - (kernel - 1 - k) * dilation;
                if (slot < 0)
                    slot += slots;
                const float* past = history.data() + static_cast<std::size_t>(slot) * in;
                for (int o = 0; o < out; ++o) {
                    float acc = 0.0f;
                    const auto base = (static_cast<std::size_t>(k) * out + o) * in;
                    for (int i = 0; i < in; ++i)
                        acc += weights[base + i] * past[i];
                    y[o] += acc;
                }
            }
            if (++head == slots)
                head = 0;
        }
    };
    struct Layer {
        Conv convolution, mixin, residual;
        bool gated = false;
        std::string activation;
        std::vector<float> z, mix, head, next;
        void configure(const ArrayConfig& c, int dilation) {
            gated = c.gated;
            activation = c.activation;
            convolution.configure(c.channels, c.gated ? 2 * c.channels : c.channels, c.kernel_size,
                                  dilation, true);
            mixin.configure(1, c.gated ? 2 * c.channels : c.channels, 1, 1, false);
            residual.configure(c.channels, c.channels, 1, 1, true);
            z.assign(static_cast<std::size_t>(c.gated ? 2 * c.channels : c.channels), 0.0f);
            mix.assign(z.size(), 0.0f);
            head.assign(c.channels, 0.0f);
            next.assign(c.channels, 0.0f);
        }
        std::size_t count() const noexcept {
            return convolution.count() + mixin.count() + residual.count();
        }
        void load(const float*& p) noexcept {
            convolution.load(p);
            mixin.load(p);
            residual.load(p);
        }
        void reset() noexcept {
            convolution.reset();
            mixin.reset();
            residual.reset();
        }
        static float activate(const std::string& a, float x) noexcept {
            if (a == "ReLU")
                return std::max(0.0f, x);
            if (a == "Hardtanh")
                return std::clamp(x, -1.0f, 1.0f);
            if (a == "Sigmoid")
                return 1.0f / (1.0f + std::exp(-x));
            if (a == "Identity")
                return x;
            return std::tanh(x);
        }
        void step(const float* input, const float* condition, float* output, float* skip) noexcept {
            convolution.step(input, z.data());
            mixin.step(condition, mix.data());
            for (std::size_t i = 0; i < z.size(); ++i)
                z[i] += mix[i];
            if (gated) {
                const auto channels = head.size();
                for (std::size_t i = 0; i < channels; ++i)
                    head[i] = activate(activation, z[i]) * activate("Sigmoid", z[channels + i]);
                residual.step(head.data(), next.data());
            } else {
                for (std::size_t i = 0; i < head.size(); ++i)
                    head[i] = activate(activation, z[i]);
                residual.step(head.data(), next.data());
            }
            for (std::size_t i = 0; i < head.size(); ++i)
                output[i] = input[i] + next[i];
            std::copy(head.begin(), head.end(), skip);
        }
    };
    struct Array {
        ArrayConfig config;
        Conv rechannel, head_rechannel;
        std::vector<Layer> layers;
        std::vector<float> current, next, accumulator, skip, head;
        void configure(const ArrayConfig& c) {
            config = c;
            rechannel.configure(c.input_size, c.channels, 1, 1, false);
            layers.resize(c.dilations.size());
            for (std::size_t i = 0; i < layers.size(); ++i)
                layers[i].configure(c, c.dilations[i]);
            head_rechannel.configure(c.channels, c.head_size, 1, 1, c.head_bias);
            current.assign(c.channels, 0.0f);
            next.assign(c.channels, 0.0f);
            accumulator.assign(c.channels, 0.0f);
            skip.assign(c.channels, 0.0f);
            head.assign(c.head_size, 0.0f);
        }
        std::size_t count() const noexcept {
            std::size_t n = rechannel.count() + head_rechannel.count();
            for (const auto& layer : layers)
                n += layer.count();
            return n;
        }
        void load(const float*& p) noexcept {
            rechannel.load(p);
            for (auto& layer : layers)
                layer.load(p);
            head_rechannel.load(p);
        }
        void reset() noexcept {
            rechannel.reset();
            for (auto& layer : layers)
                layer.reset();
            head_rechannel.reset();
        }
        void step(const float* input, const float* condition, const float* prior_head,
                  float* output, float* output_head) noexcept {
            rechannel.step(input, current.data());
            std::fill(accumulator.begin(), accumulator.end(), 0.0f);
            if (prior_head != nullptr)
                std::copy(prior_head, prior_head + accumulator.size(), accumulator.begin());
            for (auto& layer : layers) {
                layer.step(current.data(), condition, next.data(), skip.data());
                for (std::size_t i = 0; i < accumulator.size(); ++i)
                    accumulator[i] += skip[i];
                std::swap(current, next);
            }
            std::copy(current.begin(), current.end(), output);
            head_rechannel.step(accumulator.data(), output_head);
        }
        std::uint32_t receptive_field() const noexcept {
            std::uint32_t result = 0;
            for (int dilation : config.dilations)
                result += static_cast<std::uint32_t>((config.kernel_size - 1) * dilation);
            return result;
        }
    };
    struct Model {
        std::vector<Array> arrays;
        std::uint32_t receptive_field = 0;
        std::uint64_t state_bytes = 0;
        float head_scale = 1.0f;
        bool configure(const std::vector<ArrayConfig>& configs, const std::vector<float>& weights,
                       float scale, std::string* error) {
            arrays.resize(configs.size());
            for (std::size_t i = 0; i < arrays.size(); ++i)
                arrays[i].configure(configs[i]);
            std::size_t expected = 1;
            for (const auto& array : arrays) {
                expected += array.count();
                receptive_field += array.receptive_field();
            }
            if (weights.size() != expected) {
                if (error != nullptr)
                    *error = "weight count mismatch";
                return false;
            }
            const float* cursor = weights.data();
            for (auto& array : arrays)
                array.load(cursor);
            if (static_cast<std::size_t>(cursor - weights.data()) != weights.size() - 1) {
                if (error != nullptr)
                    *error = "serialized weight cursor mismatch";
                return false;
            }
            // NAM stores the runtime head scale as the final serialized
            // weight. The config value is metadata and must agree with that
            // runtime value; accepting a mismatch would silently produce a
            // different model from the GPU/CPU oracle.
            const float serialized_head_scale = *cursor++;
            if (!std::isfinite(serialized_head_scale) ||
                std::abs(serialized_head_scale - scale) > 1.0e-6f) {
                if (error != nullptr)
                    *error = "config head_scale does not match serialized runtime head_scale";
                return false;
            }
            head_scale = serialized_head_scale;
            state_bytes = 0;
            for (const auto& array : arrays)
                for (const auto& layer : array.layers)
                    state_bytes += layer.convolution.history.size() * sizeof(float);
            return true;
        }
    };

    static bool active(const choc::value::ValueView& value, const char* key) {
        return value.hasObjectMember(key) && value[key].isObject() &&
               value[key].hasObjectMember("active") &&
               value[key]["active"].getWithDefault<bool>(false);
    }
    static double number(const choc::value::ValueView& value) {
        if (value.isInt64())
            return static_cast<double>(value.getInt64());
        if (value.isInt32())
            return static_cast<double>(value.getInt32());
        if (value.isFloat64())
            return value.getFloat64();
        if (value.isFloat32())
            return value.getFloat32();
        return std::numeric_limits<double>::quiet_NaN();
    }
    static int integer(const choc::value::ValueView& value, int low, int high, bool& ok) {
        const double value_as_number = number(value);
        if (!std::isfinite(value_as_number) || value_as_number < low || value_as_number > high ||
            std::floor(value_as_number) != value_as_number) {
            ok = false;
            return low;
        }
        return static_cast<int>(value_as_number);
    }
    static float finite_float(double value, bool& ok) {
        if (!std::isfinite(value) || value > std::numeric_limits<float>::max() ||
            value < std::numeric_limits<float>::lowest()) {
            ok = false;
            return 0.0f;
        }
        return static_cast<float>(value);
    }

    float process_sample(float input) noexcept {
        float layer_input[64]{};
        float next_layer[64]{};
        float prior_head[64]{};
        float output_head[64]{};
        layer_input[0] = input;
        const float condition[1] = {input};
        const float* input_ptr = layer_input;
        const float* prior_ptr = nullptr;
        for (auto& array : arrays_) {
            array.step(input_ptr, condition, prior_ptr, next_layer, output_head);
            std::copy(next_layer, next_layer + array.config.channels, layer_input);
            std::copy(output_head, output_head + array.config.head_size, prior_head);
            input_ptr = layer_input;
            prior_ptr = prior_head;
        }
        // The serialized head scale is retained separately by load().
        return head_scale_ * output_head[0];
    }

    std::vector<Array> arrays_;
    double sample_rate_ = -1.0;
    float head_scale_ = 1.0f;
    std::uint32_t receptive_field_ = 0;
    std::uint64_t state_bytes_ = 0;
    std::size_t weights_size_ = 0;
    bool loaded_ = false;
};

/// Private StreamingModel bridge for a prepared serialized artifact. Loading is
/// control-thread-only; process_cpu only advances the already allocated causal
/// state and never parses, allocates, or touches filesystem state.
class NamTcnArtifactAdapter final : public StreamingModel {
  public:
    NamTcnArtifactAdapter(StreamingModelSpec spec, std::string artifact_path) noexcept
        : spec_(spec), artifact_path_(std::move(artifact_path)) {}

    const StreamingModelSpec& spec() const noexcept override {
        return spec_;
    }

    bool prepare(const StreamingPrepareContext& context) noexcept override {
        // Replacement is an explicit release/prepare transaction. Keep the
        // live artifact available when a caller attempts to prepare over it.
        if (prepared_)
            return false;
        if (!valid_streaming_prepare_context(context) || context.spec != &spec_ ||
            context.max_frames == 0 || spec_.input_channels != 1 || spec_.output_channels != 1)
            return false;
        std::string error;
        if (!artifact_.load(artifact_path_, &error))
            return false;
        if (artifact_.sample_rate() != static_cast<double>(spec_.sample_rate) ||
            artifact_.receptive_field() != spec_.receptive_field_samples ||
            artifact_.state_bytes() != spec_.state_bytes)
            return false;
        max_frames_ = context.max_frames;
        prepared_ = true;
        return true;
    }

    void process_cpu(const audio::BufferView<const float>& input, audio::BufferView<float>& output,
                     std::uint32_t frames, StreamingBlockStamp) noexcept override {
        if (!prepared_ || input.num_channels() != 1 || output.num_channels() != 1 ||
            frames > max_frames_ || input.num_samples() < frames || output.num_samples() < frames) {
            output.clear();
            return;
        }
        artifact_.process(input.channel_ptr(0), output.channel_ptr(0), frames);
    }

    bool quiesce() noexcept override {
        return prepared_;
    }

    void reset(std::uint64_t, StreamingResetReason) noexcept override {
        if (prepared_)
            artifact_.reset();
    }

    bool release() noexcept override {
        if (!prepared_)
            return true;
        artifact_ = NamTcnArtifact{};
        prepared_ = false;
        max_frames_ = 0;
        return true;
    }

  private:
    StreamingModelSpec spec_;
    std::string artifact_path_;
    NamTcnArtifact artifact_;
    std::uint32_t max_frames_ = 0;
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio::detail
