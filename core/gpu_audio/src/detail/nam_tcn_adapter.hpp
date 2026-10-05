#pragma once

#include "streaming_model.hpp"

#include <cstdint>
#include <limits>

namespace pulp::gpu_audio::detail {

/// Bridge for a stateful single-channel causal-convolution kernel supplied by a
/// consumer. The kernel owns parsed weights and causal state; Pulp owns
/// lifecycle and BufferView/StreamingModel admission. No model headers or
/// model format are part of the Pulp ABI.
struct NamTcnCpuKernel {
    void* state = nullptr;
    bool (*prepare)(void*, const StreamingPrepareContext&) noexcept = nullptr;
    void (*process)(void*, const float*, float*, std::uint32_t) noexcept = nullptr;
    void (*reset)(void*) noexcept = nullptr;
    bool (*quiesce)(void*) noexcept = nullptr;
    bool (*release)(void*) noexcept = nullptr;
};

class NamTcnStreamingAdapter final : public StreamingModel {
  public:
    NamTcnStreamingAdapter(StreamingModelSpec spec, NamTcnCpuKernel kernel) noexcept
        : spec_(spec), kernel_(kernel) {}

    const StreamingModelSpec& spec() const noexcept override {
        return spec_;
    }

    bool prepare(const StreamingPrepareContext& context) noexcept override {
        // A live instance owns the kernel state.  Validation or a replacement
        // attempt must never tear down that state implicitly; the owner must
        // quiesce and release it before preparing a new configuration.
        if (prepared_ || cleanup_required_)
            return false;
        if (spec_.input_channels != 1 || spec_.output_channels != 1 ||
            !valid_streaming_prepare_context(context) || context.spec != &spec_ ||
            kernel_.state == nullptr || kernel_.prepare == nullptr || kernel_.process == nullptr ||
            kernel_.reset == nullptr || kernel_.quiesce == nullptr || kernel_.release == nullptr) {
            return false;
        }

        if (!kernel_.prepare(kernel_.state, context)) {
            // A failed preparation may have reserved partial state. Release is
            // idempotent by contract so the next transaction can retry.
            cleanup_required_ = !kernel_.release(kernel_.state);
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
            buffers_overlap(input.channel_ptr(0), output.channel_ptr(0), frames)) {
            output.clear();
            return;
        }
        kernel_.process(kernel_.state, input.channel_ptr(0), output.channel_ptr(0), frames);
    }

    bool quiesce() noexcept override {
        return kernel_.quiesce != nullptr && kernel_.quiesce(kernel_.state);
    }

    void reset(std::uint64_t, StreamingResetReason) noexcept override {
        if (prepared_ && kernel_.reset != nullptr)
            kernel_.reset(kernel_.state);
    }

    bool release() noexcept override {
        if (!prepared_ && !cleanup_required_)
            return true;
        if (kernel_.release == nullptr)
            return false;
        if (!kernel_.release(kernel_.state))
            return false;
        prepared_ = false;
        max_frames_ = 0;
        cleanup_required_ = false;
        return true;
    }

  private:
    static bool buffers_overlap(const float* input, float* output, std::uint32_t frames) noexcept {
        if (frames == 0)
            return false;
        const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(float);
        const auto input_begin = reinterpret_cast<std::uintptr_t>(input);
        const auto output_begin = reinterpret_cast<std::uintptr_t>(output);
        const auto max_address = std::numeric_limits<std::uintptr_t>::max();
        if (bytes > max_address - input_begin || bytes > max_address - output_begin)
            return true;
        const auto input_end = input_begin + bytes;
        const auto output_end = output_begin + bytes;
        return input_begin < output_end && output_begin < input_end;
    }

    StreamingModelSpec spec_;
    NamTcnCpuKernel kernel_;
    std::uint32_t max_frames_ = 0;
    bool prepared_ = false;
    bool cleanup_required_ = false;
};

} // namespace pulp::gpu_audio::detail
