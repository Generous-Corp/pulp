#pragma once

#include "compact_ssm_cpu_adapter.hpp"
#include "nam_tcn_adapter.hpp"
#include "neural_model_manifest.hpp"
#include "recurrent_cpu_adapter.hpp"
#include "streaming_model.hpp"
#include "tcn_cpu_adapter.hpp"
#include "wavenet_streaming_backend.hpp"

#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::gpu_audio {

/// Host-built model artifact. Strings are owned here so every spec and
/// prepare-context view stays valid for the model's lifetime; the object is
/// never moved after construction (it lives behind a shared_ptr).
class GpuNeuralModel {
  public:
    GpuNeuralModel() = default;
    GpuNeuralModel(const GpuNeuralModel&) = delete;
    GpuNeuralModel& operator=(const GpuNeuralModel&) = delete;

    NeuralFamily family = NeuralFamily::Unknown;
    std::string model_id, architecture, model_version, artifact_id, artifact_sha256, license,
        runtime, state_schema;
    std::uint32_t sample_rate = 0;
    std::uint32_t block_size = 0;
    /// Adapter objects the public factory created. Declared before the
    /// channel models, which point into them, so they are destroyed last.
    std::vector<std::shared_ptr<void>> owned;
    /// One independent causal state per audio channel.
    std::vector<std::unique_ptr<detail::StreamingModel>> channel_models;
    /// Present only for families with a GPU kernel.
    std::unique_ptr<detail::StreamingBackend> backend;
    detail::StreamingModelSpec backend_spec{};
    /// Set by the first node that binds the model; a model's causal state
    /// cannot be shared between two nodes.
    std::atomic<bool> bound{false};

    std::uint32_t channels() const noexcept {
        return static_cast<std::uint32_t>(channel_models.size());
    }
    bool production_backend() const noexcept {
        const auto* wavenet = dynamic_cast<const detail::WaveNetStreamingBackend*>(backend.get());
        return wavenet != nullptr && wavenet->production_provider();
    }
};

namespace detail {

/// Consumer-owned per-channel CPU kernel. Field-for-field identical to the
/// TCN, NAM-TCN, recurrent and SSM adapter kernels; each channel supplies its
/// own `state` so channels never share activations.
struct NeuralCpuKernel {
    void* state = nullptr;
    bool (*prepare)(void*, const StreamingPrepareContext&) noexcept = nullptr;
    void (*process)(void*, const float*, float*, std::uint32_t) noexcept = nullptr;
    void (*reset)(void*) noexcept = nullptr;
    bool (*quiesce)(void*) noexcept = nullptr;
    bool (*release)(void*) noexcept = nullptr;
};

enum class NeuralModelFactoryError : std::uint8_t {
    None,
    UnsupportedFamily,
    InvalidManifest,
    InvalidBlockSize,
    MissingKernels,
    ShapeMismatch,
    UnsupportedBackend,
    AllocationFailed,
};

/// Control-plane request. Manifest strings are copied into the model.
struct NeuralModelRequest {
    NeuralFamily family = NeuralFamily::Unknown;
    NeuralModelManifest manifest{};
    NeuralModelUse use = NeuralModelUse::Shipped;
    std::uint32_t block_size = 0;
    /// Family shape; only the field for `family` is read. NAM-TCN
    /// (architecture "nam.tcn"), WaveNet and Custom kernels carry their own
    /// topology and use no shape here.
    TcnCpuShape tcn{};
    RecurrentCpuShape recurrent{};
    CompactSsmCpuShape ssm{};
    /// One kernel per audio channel.
    std::span<const NeuralCpuKernel> channel_kernels{};
    /// Optional GPU backend; accepted only for families with a GPU kernel.
    std::unique_ptr<StreamingBackend> backend{};
};

struct NeuralModelBuild {
    GpuNeuralModelHandle model;
    NeuralModelFactoryError error = NeuralModelFactoryError::None;
    explicit operator bool() const noexcept {
        return model != nullptr && error == NeuralModelFactoryError::None;
    }
};

/// Families with a GPU kernel. Recurrent and SSM recurrences are one
/// dependent step per sample, and single-instance TCN does not beat the CPU,
/// so those run the CPU-only contract.
constexpr bool neural_family_has_gpu_kernel(NeuralFamily family) noexcept {
    return family == NeuralFamily::WaveNet;
}

inline constexpr std::string_view kNamTcnArchitecture = "nam.tcn";

/// Host-only. Selects the StreamingModel adapter for the manifest's family
/// and binds the optional GPU backend. Never call from the audio thread.
inline NeuralModelBuild make_neural_model(NeuralModelRequest&& request) {
    using Error = NeuralModelFactoryError;
    const auto family = request.family;
    const auto& m = request.manifest;
    if (family == NeuralFamily::Unknown)
        return {{}, Error::UnsupportedFamily};
    if (!validate_neural_model_manifest(m, request.use).accepted())
        return {{}, Error::InvalidManifest};
    if (request.block_size == 0 || request.block_size > 4096)
        return {{}, Error::InvalidBlockSize};
    if (request.channel_kernels.empty() || request.channel_kernels.size() > 64)
        return {{}, Error::MissingKernels};
    for (const auto& kernel : request.channel_kernels)
        if (kernel.state == nullptr || kernel.prepare == nullptr || kernel.process == nullptr ||
            kernel.reset == nullptr || kernel.quiesce == nullptr || kernel.release == nullptr)
            return {{}, Error::MissingKernels};
    if (request.backend && !neural_family_has_gpu_kernel(family))
        return {{}, Error::UnsupportedBackend};

    const bool nam_tcn = family == NeuralFamily::Tcn && m.architecture == kNamTcnArchitecture;
    std::uint32_t receptive_field = 0;
    switch (family) {
    case NeuralFamily::Tcn:
        if (!nam_tcn) {
            if (!valid_tcn_cpu_shape(request.tcn) ||
                m.state_bytes != tcn_cpu_state_bytes(request.tcn) ||
                m.state_schema != "causal-tcn-v1" || m.state_schema_version != 1)
                return {{}, Error::ShapeMismatch};
            receptive_field = tcn_cpu_receptive_field(request.tcn);
        }
        break;
    case NeuralFamily::Ssm:
        if (!valid_compact_ssm_cpu_shape(request.ssm) ||
            m.state_bytes != compact_ssm_cpu_state_bytes(request.ssm) ||
            m.state_schema != "compact-ssm-diagonal-v1" || m.state_schema_version != 1)
            return {{}, Error::ShapeMismatch};
        break;
    case NeuralFamily::Lstm:
    case NeuralFamily::Gru: {
        const auto expected =
            family == NeuralFamily::Lstm ? RecurrentFamily::Lstm : RecurrentFamily::Gru;
        if (request.recurrent.family != expected || !valid_recurrent_cpu_shape(request.recurrent) ||
            m.state_bytes != recurrent_state_bytes(request.recurrent) ||
            m.state_schema != recurrent_state_schema(expected) || m.state_schema_version != 1)
            return {{}, Error::ShapeMismatch};
        break;
    }
    case NeuralFamily::WaveNet:
    case NeuralFamily::Custom:
        break;
    case NeuralFamily::Unknown:
        return {{}, Error::UnsupportedFamily};
    }

    std::shared_ptr<GpuNeuralModel> model;
    try {
        model = std::make_shared<GpuNeuralModel>();
        model->family = family;
        model->model_id = m.model_id;
        model->architecture = m.architecture;
        model->model_version = m.model_version;
        model->artifact_id = m.artifact_id;
        model->artifact_sha256 = m.artifact_sha256;
        model->license = m.license;
        model->runtime = m.runtime;
        model->state_schema = m.state_schema;
        model->sample_rate = m.sample_rate_hz;
        model->block_size = request.block_size;

        const StreamingModelSpec spec{.model_id = model->model_id,
                                      .architecture = model->architecture,
                                      .model_version = model->model_version,
                                      .weights_hash = model->artifact_sha256,
                                      .runtime_hash = model->runtime,
                                      .input_channels = 1,
                                      .output_channels = 1,
                                      .sample_rate = model->sample_rate,
                                      .block_size = model->block_size,
                                      .feature_rate = model->sample_rate,
                                      .intrinsic_latency_samples = 0,
                                      .receptive_field_samples = receptive_field,
                                      .state_bytes = m.state_bytes,
                                      .state_schema = model->state_schema,
                                      .state_schema_version = m.state_schema_version,
                                      .deterministic = true};
        if (!validate_streaming_model_spec(spec).accepted())
            return {{}, Error::InvalidManifest};

        const auto channels = static_cast<std::uint32_t>(request.channel_kernels.size());
        model->backend_spec = spec;
        model->backend_spec.input_channels = channels;
        model->backend_spec.output_channels = channels;

        model->channel_models.reserve(channels);
        for (const auto& k : request.channel_kernels) {
            std::unique_ptr<StreamingModel> adapter;
            switch (family) {
            case NeuralFamily::Tcn:
                if (nam_tcn)
                    adapter = std::make_unique<NamTcnStreamingAdapter>(
                        spec, NamTcnCpuKernel{k.state, k.prepare, k.process, k.reset, k.quiesce,
                                              k.release});
                else
                    adapter = std::make_unique<TcnCpuAdapter>(
                        spec, request.tcn,
                        TcnCpuArtifact{.artifact_id = model->artifact_id,
                                       .artifact_sha256 = model->artifact_sha256,
                                       .license_id = model->license,
                                       .tensor_schema = model->state_schema,
                                       .tensor_schema_version = m.state_schema_version},
                        TcnCpuKernel{k.state, k.prepare, k.process, k.reset, k.quiesce, k.release});
                break;
            case NeuralFamily::Ssm:
                adapter = std::make_unique<CompactSsmCpuAdapter>(
                    spec, request.ssm,
                    CompactSsmCpuArtifact{.artifact_id = model->artifact_id,
                                          .artifact_sha256 = model->artifact_sha256,
                                          .license_id = model->license,
                                          .tensor_schema = model->state_schema,
                                          .tensor_schema_version = m.state_schema_version},
                    CompactSsmCpuKernel{k.state, k.prepare, k.process, k.reset, k.quiesce,
                                        k.release});
                break;
            case NeuralFamily::Lstm:
            case NeuralFamily::Gru:
                adapter = std::make_unique<RecurrentCpuAdapter>(
                    spec, request.recurrent,
                    RecurrentCpuKernel{k.state, k.prepare, k.process, k.reset, k.quiesce,
                                       k.release});
                break;
            case NeuralFamily::WaveNet:
            case NeuralFamily::Custom:
                // The consumer kernel owns the topology; the NAM-TCN bridge is
                // the generic single-channel stateful-kernel adapter.
                adapter = std::make_unique<NamTcnStreamingAdapter>(
                    spec,
                    NamTcnCpuKernel{k.state, k.prepare, k.process, k.reset, k.quiesce, k.release});
                break;
            case NeuralFamily::Unknown:
                return {{}, Error::UnsupportedFamily};
            }
            model->channel_models.push_back(std::move(adapter));
        }
    } catch (...) {
        return {{}, Error::AllocationFailed};
    }
    model->backend = std::move(request.backend);
    return {std::move(model), Error::None};
}

/// Production WaveNet backend over the authenticated GpuWaveNetSession.
inline std::unique_ptr<StreamingBackend>
make_wavenet_streaming_backend(const GpuWaveNetSession::Config& session, std::uint32_t channels,
                               std::uint32_t max_inflight) {
    return std::make_unique<WaveNetStreamingBackend>(WaveNetStreamingBackend::Config{
        .session = session, .channels = channels, .max_inflight = max_inflight});
}

} // namespace detail
} // namespace pulp::gpu_audio
