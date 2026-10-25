#include "detail/gpu_neural_model.hpp"

#include <pulp/gpu_audio/gpu_neural_model.hpp>

#include <memory>
#include <vector>

namespace pulp::gpu_audio {
namespace {

// Adapts a public kernel to the private per-channel seam. Owned by the model.
struct PublicKernel {
    GpuNeuralCpuKernel kernel;
};

detail::NeuralCpuKernel bind(PublicKernel& owner) noexcept {
    return {&owner,
            [](void* self, const detail::StreamingPrepareContext&) noexcept {
                auto& k = static_cast<PublicKernel*>(self)->kernel;
                return k.prepare(k.state);
            },
            [](void* self, const float* in, float* out, std::uint32_t frames) noexcept {
                auto& k = static_cast<PublicKernel*>(self)->kernel;
                k.process(k.state, in, out, frames);
            },
            [](void* self) noexcept {
                auto& k = static_cast<PublicKernel*>(self)->kernel;
                k.reset(k.state);
            },
            [](void*) noexcept { return true; },
            [](void*) noexcept { return true; }};
}

GpuNeuralModelError public_error(detail::NeuralModelFactoryError error) noexcept {
    using E = detail::NeuralModelFactoryError;
    switch (error) {
    case E::None:
        return GpuNeuralModelError::None;
    case E::UnsupportedFamily:
        return GpuNeuralModelError::UnsupportedFamily;
    case E::InvalidManifest:
        return GpuNeuralModelError::InvalidManifest;
    case E::InvalidBlockSize:
        return GpuNeuralModelError::InvalidBlockSize;
    case E::MissingKernels:
        return GpuNeuralModelError::MissingKernels;
    case E::ShapeMismatch:
        return GpuNeuralModelError::ShapeMismatch;
    case E::UnsupportedBackend:
        return GpuNeuralModelError::UnsupportedBackend;
    case E::AllocationFailed:
        return GpuNeuralModelError::AllocationFailed;
    }
    return GpuNeuralModelError::AllocationFailed;
}

} // namespace

GpuNeuralModelResult make_gpu_neural_model(const GpuNeuralModelRequest& request) noexcept {
    try {
        const auto& m = request.manifest;
        detail::NeuralModelRequest inner;
        inner.family = request.family;
        inner.use = m.local_research ? detail::NeuralModelUse::LocalResearch
                                     : detail::NeuralModelUse::Shipped;
        inner.block_size = request.block_size;
        inner.manifest = {.model_id = m.model_id,
                          .architecture = m.architecture,
                          .model_version = m.model_version,
                          .artifact_id = m.artifact_id,
                          .artifact_sha256 = m.artifact_sha256,
                          .license = m.license,
                          .runtime = m.runtime,
                          .artifact_size_bytes = m.artifact_size_bytes,
                          .sample_rate_hz = m.sample_rate_hz,
                          .redistributable = m.redistributable};
        // The state schema is a function of the family's shape, so callers
        // describe the shape and the factory derives the schema.
        const auto& shape = request.shape;
        switch (request.family) {
        case NeuralFamily::Tcn:
            if (m.architecture != detail::kNamTcnArchitecture) {
                inner.tcn = {.channels = 1,
                             .kernel_size = shape.tcn_kernel_size,
                             .dilation = shape.tcn_dilation,
                             .layers = 1};
                inner.manifest.state_bytes = detail::tcn_cpu_state_bytes(inner.tcn);
                inner.manifest.state_schema = "causal-tcn-v1";
            }
            break;
        case NeuralFamily::Ssm:
            inner.ssm = {
                .state_size = shape.ssm_state_size, .input_size = 1, .output_size = 1, .layers = 1};
            inner.manifest.state_bytes = detail::compact_ssm_cpu_state_bytes(inner.ssm);
            inner.manifest.state_schema = "compact-ssm-diagonal-v1";
            break;
        case NeuralFamily::Lstm:
        case NeuralFamily::Gru: {
            const auto family = request.family == NeuralFamily::Lstm ? detail::RecurrentFamily::Lstm
                                                                     : detail::RecurrentFamily::Gru;
            inner.recurrent = {.family = family,
                               .input_size = 1,
                               .hidden_size = shape.recurrent_hidden_size,
                               .layers = 1,
                               .directions = 1,
                               .output_size = 1};
            inner.manifest.state_bytes = detail::recurrent_state_bytes(inner.recurrent);
            inner.manifest.state_schema = detail::recurrent_state_schema(family);
            break;
        }
        default:
            break;
        }
        if (inner.manifest.state_bytes == 0)
            inner.manifest.state_schema = {};
        inner.manifest.state_schema_version = inner.manifest.state_bytes != 0 ? 1u : 0u;
        // A missing shape leaves zero state bytes for a family that needs
        // them; the adapter checks would reject it later, so say so now.
        if ((request.family == NeuralFamily::Ssm || request.family == NeuralFamily::Lstm ||
             request.family == NeuralFamily::Gru ||
             (request.family == NeuralFamily::Tcn &&
              m.architecture != detail::kNamTcnArchitecture)) &&
            inner.manifest.state_bytes == 0)
            return {{}, GpuNeuralModelError::ShapeMismatch};

        std::vector<std::shared_ptr<void>> owned;
        std::vector<detail::NeuralCpuKernel> seams;
        owned.reserve(request.channel_kernels.size());
        seams.reserve(request.channel_kernels.size());
        for (const auto& kernel : request.channel_kernels) {
            if (kernel.state == nullptr || kernel.prepare == nullptr || kernel.process == nullptr ||
                kernel.reset == nullptr)
                return {{}, GpuNeuralModelError::MissingKernels};
            auto adapter = std::make_shared<PublicKernel>(PublicKernel{kernel});
            seams.push_back(bind(*adapter));
            owned.push_back(std::move(adapter));
        }
        inner.channel_kernels = seams;
        if (request.wavenet_gpu != nullptr) {
            if (request.family != NeuralFamily::WaveNet)
                return {{}, GpuNeuralModelError::UnsupportedBackend};
            inner.backend = detail::make_wavenet_streaming_backend(
                *request.wavenet_gpu, static_cast<std::uint32_t>(seams.size()),
                request.gpu_max_inflight);
        }
        auto build = detail::make_neural_model(std::move(inner));
        if (!build)
            return {{}, public_error(build.error)};
        build.model->owned = std::move(owned);
        return {std::move(build.model), GpuNeuralModelError::None};
    } catch (...) {
        return {{}, GpuNeuralModelError::AllocationFailed};
    }
}

} // namespace pulp::gpu_audio
