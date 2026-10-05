#pragma once

#include "streaming_model.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pulp::gpu_audio::detail {

enum class RecurrentFamily : std::uint8_t { Lstm, Gru };

struct RecurrentCpuShape {
    RecurrentFamily family = RecurrentFamily::Lstm;
    std::uint32_t input_size = 0;
    std::uint32_t hidden_size = 0;
    std::uint32_t layers = 0;
    std::uint32_t directions = 0;
    std::uint32_t output_size = 0;
};

constexpr std::size_t recurrent_gate_count(RecurrentFamily family) noexcept {
    return family == RecurrentFamily::Lstm ? 4u : 3u;
}

constexpr std::size_t recurrent_state_count(RecurrentFamily family) noexcept {
    return family == RecurrentFamily::Lstm ? 2u : 1u;
}

constexpr bool valid_recurrent_cpu_shape(const RecurrentCpuShape& shape) noexcept {
    // The first packet is deliberately one-layer, causal, and scalar I/O.
    return shape.input_size == 1 && shape.hidden_size > 0 && shape.hidden_size <= 256 &&
           shape.layers == 1 && shape.directions == 1 && shape.output_size == 1;
}

constexpr std::size_t recurrent_state_bytes(const RecurrentCpuShape& shape) noexcept {
    if (!valid_recurrent_cpu_shape(shape))
        return 0;
    const auto floats = (recurrent_state_count(shape.family) +
                         recurrent_gate_count(shape.family)) * shape.hidden_size;
    return floats * sizeof(float);
}

constexpr std::string_view recurrent_state_schema(RecurrentFamily family) noexcept {
    return family == RecurrentFamily::Lstm ? "recurrent-lstm-v1" : "recurrent-gru-v1";
}

/// Consumer-owned, prepared CPU kernel. The callbacks are control-thread-only
/// except process(), which must mutate only storage acquired by prepare().
struct RecurrentCpuKernel {
    void* state = nullptr;
    bool (*prepare)(void*, const StreamingPrepareContext&) noexcept = nullptr;
    void (*process)(void*, const float*, float*, std::uint32_t) noexcept = nullptr;
    void (*reset)(void*) noexcept = nullptr;
    bool (*quiesce)(void*) noexcept = nullptr;
    bool (*release)(void*) noexcept = nullptr;
};

/// Private model-neutral adapter for synthetic recurrent CPU kernels. It does
/// not parse artifacts or publish provider capabilities; those remain outside
/// this packet. All shape validation and preparation happen before publication.
class RecurrentCpuAdapter final : public StreamingModel {
  public:
    RecurrentCpuAdapter(StreamingModelSpec spec, RecurrentCpuShape shape,
                        RecurrentCpuKernel kernel) noexcept
        : spec_(spec), shape_(shape), kernel_(kernel) {}

    const StreamingModelSpec& spec() const noexcept override {
        return spec_;
    }

    const RecurrentCpuShape& shape() const noexcept {
        return shape_;
    }

    bool prepare(const StreamingPrepareContext& context) noexcept override {
        prepared_ = false;
        max_frames_ = 0;
        if (!valid_recurrent_cpu_shape(shape_) || recurrent_state_bytes(shape_) == 0 ||
            !valid_streaming_prepare_context(context) || context.spec != &spec_ ||
            context.max_frames > kMaxSupportedFrames || spec_.input_channels != 1 ||
            spec_.output_channels != 1 || spec_.state_bytes != recurrent_state_bytes(shape_) ||
            spec_.state_schema != recurrent_state_schema(shape_.family) ||
            spec_.state_schema_version != 1 || kernel_.state == nullptr ||
            kernel_.prepare == nullptr || kernel_.process == nullptr || kernel_.reset == nullptr ||
            kernel_.quiesce == nullptr || kernel_.release == nullptr)
            return false;

        if (!kernel_.prepare(kernel_.state, context)) {
            // A failed preparation may have reserved partial state. Release is
            // idempotent by contract so the next transaction can retry.
            (void)kernel_.release(kernel_.state);
            return false;
        }
        max_frames_ = context.max_frames;
        prepared_ = true;
        return true;
    }

    void process_cpu(const audio::BufferView<const float>& input, audio::BufferView<float>& output,
                     std::uint32_t frames, StreamingBlockStamp) noexcept override {
        if (!prepared_ || input.num_channels() != 1 || output.num_channels() != 1 ||
            frames > max_frames_ || input.num_samples() < frames || output.num_samples() < frames ||
            input.channel_ptr(0) == output.channel_ptr(0)) {
            output.clear();
            return;
        }
        kernel_.process(kernel_.state, input.channel_ptr(0), output.channel_ptr(0), frames);
    }

    bool quiesce() noexcept override {
        return kernel_.quiesce != nullptr && kernel_.quiesce(kernel_.state);
    }

    void reset(std::uint64_t, StreamingResetReason) noexcept override {
        if (prepared_)
            kernel_.reset(kernel_.state);
    }

    bool release() noexcept override {
        if (kernel_.release == nullptr)
            return false;
        if (!kernel_.release(kernel_.state))
            return false;
        prepared_ = false;
        max_frames_ = 0;
        return true;
    }

  private:
    static constexpr std::uint32_t kMaxSupportedFrames = 4096;

    StreamingModelSpec spec_;
    RecurrentCpuShape shape_;
    RecurrentCpuKernel kernel_;
    std::uint32_t max_frames_ = 0;
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio::detail
