#include <pulp/gpu_audio/gpu_wavenet.hpp>

#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/shared_io_program_session.hpp"
#endif

#include <cstring>
#include <utility>
#include <vector>

namespace pulp::gpu_audio {

struct GpuWaveNetSession::Impl {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    std::unique_ptr<detail::SharedIoProgramSession> session;
#endif
    std::uint32_t block_size = 0;
};

GpuWaveNetSession::GpuWaveNetSession(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuWaveNetSession::~GpuWaveNetSession() {
    (void)release();
}

GpuWaveNetSession::CreateResult GpuWaveNetSession::create(const Config& config) noexcept {
    CreateResult result;
    const auto validation = validate_gpu_wavenet_descriptor(config.descriptor);
    if (!validation.accepted()) {
        result.error = GpuWaveNetSessionError::InvalidDescriptor;
        return result;
    }
    if (config.weights.size() != config.descriptor.weight_count) {
        result.error = GpuWaveNetSessionError::InvalidWeights;
        return result;
    }
    if (config.slots < 2) {
        result.error = GpuWaveNetSessionError::PreparationFailed;
        return result;
    }

#if !defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    result.error = GpuWaveNetSessionError::ProviderUnavailable;
    return result;
#else
#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif
    try {
        std::vector<std::vector<std::uint32_t>> dilations;
        std::vector<detail::DawnSharedIoWavenetLayerSpec> arrays;
        dilations.reserve(config.descriptor.layers.size());
        arrays.reserve(config.descriptor.layers.size());
        for (const auto& layer : config.descriptor.layers) {
            if (layer.dilations.empty())
                dilations.push_back({layer.dilation});
            else
                dilations.emplace_back(layer.dilations.begin(), layer.dilations.end());
            arrays.push_back({.input_size = layer.input_size,
                              .condition_size = layer.condition_size,
                              .channels = layer.channels,
                              .kernel = layer.kernel,
                              .head_size = layer.head_size,
                              .gated = layer.gated ? 1u : 0u,
                              .head_bias = layer.head_bias ? 1u : 0u,
                              .dilations = dilations.back()});
        }

        const detail::DawnSharedIoWavenetProgramSpec spec{
            .block_size = config.descriptor.block_size,
            .head_scale = config.descriptor.head_scale,
            .stream_instances = config.descriptor.stream_instances,
            .arrays = arrays,
            .weights = config.weights,
        };
        if (!detail::validate_dawn_shared_io_wavenet_spec(spec).accepted()) {
            result.error = GpuWaveNetSessionError::InvalidDescriptor;
            return result;
        }

        auto created = detail::DawnSharedIoProvider::create(
            {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA});
        if (!created.provider) {
            result.error = GpuWaveNetSessionError::ProviderUnavailable;
            return result;
        }
        auto program = created.provider->make_wavenet_program(spec);
        if (!program) {
            result.error = GpuWaveNetSessionError::ProgramUnavailable;
            return result;
        }

        auto impl = std::make_unique<Impl>();
        impl->block_size = config.descriptor.block_size;
        impl->session = std::make_unique<detail::SharedIoProgramSession>();
        const auto bytes = static_cast<std::size_t>(config.descriptor.block_size) * sizeof(float);
        if (!impl->session->prepare(
                {std::move(created.provider), std::move(program)},
                {.slots = config.slots,
                 .input_bytes_per_slot = bytes,
                 .output_bytes_per_slot = bytes})) {
            result.error = GpuWaveNetSessionError::PreparationFailed;
            // Keep the owner alive so its provider can retry a physical drain
            // barrier if preparation had already allocated shared resources.
            result.session = std::unique_ptr<GpuWaveNetSession>(
                new GpuWaveNetSession(std::move(impl)));
            return result;
        }
        result.session = std::unique_ptr<GpuWaveNetSession>(
            new GpuWaveNetSession(std::move(impl)));
        result.error = GpuWaveNetSessionError::None;
        return result;
    } catch (...) {
        result.error = GpuWaveNetSessionError::PreparationFailed;
        return result;
    }
#endif
}

bool GpuWaveNetSession::prepared() const noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    return impl_ && impl_->session && impl_->session->prepared();
#else
    return false;
#endif
}

std::uint32_t GpuWaveNetSession::block_size() const noexcept {
    return impl_ ? impl_->block_size : 0;
}

bool GpuWaveNetSession::submit_block(std::span<const float> input, std::uint64_t sequence,
                                     std::uint64_t deadline_ns) noexcept {
#if !defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    (void)input;
    (void)sequence;
    (void)deadline_ns;
    return false;
#else
    if (!prepared() || input.size() != block_size())
        return false;
    auto lease = impl_->session->acquire_input(sequence, deadline_ns);
    if (!lease || lease->bytes.size() != input.size() * sizeof(float))
        return false;
    std::memcpy(lease->bytes.data(), input.data(), lease->bytes.size());
    const detail::SharedIoProgramSession::SubmitToken token{lease->token, deadline_ns};
    if (impl_->session->submit(token))
        return true;
    (void)impl_->session->cancel(token);
    return false;
#endif
}

std::size_t GpuWaveNetSession::service(std::uint64_t now_ns) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (prepared())
        return impl_->session->service(now_ns);
#else
    (void)now_ns;
#endif
    return 0;
}

std::optional<GpuWaveNetBlockResult>
GpuWaveNetSession::receive(std::span<float> output) noexcept {
#if !defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    (void)output;
    return std::nullopt;
#else
    if (!prepared() || output.size() < block_size())
        return std::nullopt;
    const auto completion = impl_->session->pop_completion();
    if (!completion)
        return std::nullopt;

    GpuWaveNetBlockResult result{
        .sequence = completion->token.slot.stream_sequence,
        .status = GpuWaveNetBlockStatus::ProviderFailed,
        .late = completion->late,
    };
    if (completion->status != detail::SharedIoArena::CompletionStatus::RetiredSuccess) {
        (void)impl_->session->discard_completion(*completion);
        return result;
    }

    auto lease = impl_->session->acquire_output(*completion);
    const auto output_bytes = static_cast<std::size_t>(block_size()) * sizeof(float);
    if (!lease || lease->bytes.size() < output_bytes) {
        if (lease)
            (void)impl_->session->release_output({lease->token});
        return result;
    }
    std::memcpy(output.data(), lease->bytes.data(), output_bytes);
    const bool released = impl_->session->release_output({lease->token});
    if (released)
        result.status = GpuWaveNetBlockStatus::GpuDelivered;
    return result;
#endif
}

bool GpuWaveNetSession::release() noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (!impl_ || !impl_->session)
        return true;
    return impl_->session->release();
#else
    return true;
#endif
}

} // namespace pulp::gpu_audio
