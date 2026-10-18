#pragma once

#include "streaming_model.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace pulp::gpu_audio::detail {

/// Private, scalar, diagonal state-space shape. This is a contract fixture,
/// not a model-format parser or a provider capability.
struct CompactSsmCpuShape {
    std::uint32_t state_size = 0;
    std::uint32_t input_size = 0;
    std::uint32_t output_size = 0;
    std::uint32_t layers = 0;
};

constexpr bool valid_compact_ssm_cpu_shape(const CompactSsmCpuShape& shape) noexcept {
    return shape.state_size > 0 && shape.state_size <= 256 && shape.input_size == 1 &&
           shape.output_size == 1 && shape.layers == 1;
}

constexpr std::size_t compact_ssm_cpu_state_bytes(const CompactSsmCpuShape& shape) noexcept {
    if (!valid_compact_ssm_cpu_shape(shape))
        return 0;
    return static_cast<std::size_t>(shape.state_size) * sizeof(float);
}

struct CompactSsmCpuArtifact {
    std::string_view artifact_id{};
    std::string_view artifact_sha256{};
    std::string_view license_id{};
    std::string_view tensor_schema{};
    std::uint32_t tensor_schema_version = 0;
};

constexpr bool valid_compact_ssm_cpu_artifact(const CompactSsmCpuArtifact& artifact) noexcept {
    return !artifact.artifact_id.empty() && !artifact.artifact_sha256.empty() &&
           !artifact.license_id.empty() && artifact.tensor_schema == "compact-ssm-diagonal-v1" &&
           artifact.tensor_schema_version == 1;
}

/// Consumer-owned prepared kernel. Only process() is callback-safe.
struct CompactSsmCpuKernel {
    void* state = nullptr;
    bool (*prepare)(void*, const StreamingPrepareContext&) noexcept = nullptr;
    void (*process)(void*, const float*, float*, std::uint32_t) noexcept = nullptr;
    void (*reset)(void*) noexcept = nullptr;
    bool (*quiesce)(void*) noexcept = nullptr;
    bool (*release)(void*) noexcept = nullptr;
};

enum class CompactSsmCpuPrepareError : std::uint8_t { None, InvalidContract, ArtifactMismatch };

class CompactSsmCpuAdapter final : public StreamingModel {
  public:
    CompactSsmCpuAdapter(StreamingModelSpec spec, CompactSsmCpuShape shape,
                         CompactSsmCpuArtifact artifact, CompactSsmCpuKernel kernel) noexcept
        : spec_(spec), shape_(shape), artifact_(artifact), kernel_(kernel) {}

    const StreamingModelSpec& spec() const noexcept override {
        return spec_;
    }
    const CompactSsmCpuShape& shape() const noexcept {
        return shape_;
    }
    const CompactSsmCpuArtifact& artifact() const noexcept {
        return artifact_;
    }
    CompactSsmCpuPrepareError last_prepare_error() const noexcept {
        return last_error_;
    }

    bool prepare(const StreamingPrepareContext& context) noexcept override {
        last_error_ = CompactSsmCpuPrepareError::None;
        if (prepared_ || cleanup_required_ || !valid_compact_ssm_cpu_shape(shape_) ||
            !valid_compact_ssm_cpu_artifact(artifact_) ||
            !valid_streaming_prepare_context(context) || context.spec != &spec_ ||
            context.max_frames > kMaxSupportedFrames || spec_.input_channels != 1 ||
            spec_.output_channels != 1 ||
            spec_.state_bytes != compact_ssm_cpu_state_bytes(shape_) ||
            spec_.state_schema != "compact-ssm-diagonal-v1" || spec_.state_schema_version != 1 ||
            kernel_.state == nullptr || kernel_.prepare == nullptr || kernel_.process == nullptr ||
            kernel_.reset == nullptr || kernel_.quiesce == nullptr || kernel_.release == nullptr)
            return fail(CompactSsmCpuPrepareError::InvalidContract);
        if (context.artifact_id != artifact_.artifact_id ||
            context.artifact_hash != artifact_.artifact_sha256)
            return fail(CompactSsmCpuPrepareError::ArtifactMismatch);
        if (!kernel_.prepare(kernel_.state, context)) {
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
            overlap(input.channel_ptr(0), output.channel_ptr(0), frames)) {
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
        if (!prepared_ && !cleanup_required_)
            return true;
        if (kernel_.release == nullptr || !kernel_.release(kernel_.state))
            return false;
        prepared_ = false;
        cleanup_required_ = false;
        max_frames_ = 0;
        return true;
    }

  private:
    static constexpr std::uint32_t kMaxSupportedFrames = 4096;
    bool fail(CompactSsmCpuPrepareError error) noexcept {
        last_error_ = error;
        return false;
    }
    static bool overlap(const float* input, float* output, std::uint32_t frames) noexcept {
        if (frames == 0)
            return false;
        const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(float);
        const auto in = reinterpret_cast<std::uintptr_t>(input);
        const auto out = reinterpret_cast<std::uintptr_t>(output);
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - in ||
            bytes > std::numeric_limits<std::uintptr_t>::max() - out)
            return true;
        return in < out + bytes && out < in + bytes;
    }
    StreamingModelSpec spec_;
    CompactSsmCpuShape shape_;
    CompactSsmCpuArtifact artifact_;
    CompactSsmCpuKernel kernel_;
    std::uint32_t max_frames_ = 0;
    bool prepared_ = false;
    bool cleanup_required_ = false;
    CompactSsmCpuPrepareError last_error_ = CompactSsmCpuPrepareError::None;
};

} // namespace pulp::gpu_audio::detail
