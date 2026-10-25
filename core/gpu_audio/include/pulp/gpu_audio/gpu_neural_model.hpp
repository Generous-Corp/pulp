#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>
#include <pulp/gpu_audio/gpu_wavenet.hpp>

namespace pulp::gpu_audio {

/// One channel's CPU model step, supplied and owned by the caller. The node
/// keeps one kernel per audio channel and never shares state between them.
/// `state` must stay valid, and its storage allocated, until the model and
/// every node that bound it are destroyed.
struct GpuNeuralCpuKernel {
    void* state = nullptr;
    /// Host thread, before processing starts: clear the causal state.
    /// Returning false refuses preparation.
    bool (*prepare)(void* state) noexcept = nullptr;
    /// Runs on the audio thread (Inline shadow, CPU-only families) or on the
    /// shadow thread (Worker). It must not allocate, lock or block. Input is
    /// always finite: the node replaces NaN and infinities with zero first.
    void (*process)(void* state, const float* input, float* output,
                    std::uint32_t frames) noexcept = nullptr;
    /// Restarts the causal state; called off the audio thread.
    void (*reset)(void* state) noexcept = nullptr;
};

/// Provenance and compatibility metadata for a model artifact. String views
/// are copied into the model.
struct GpuNeuralModelManifest {
    std::string_view model_id{};
    /// Free-form, except that a Tcn model whose architecture is "nam.tcn"
    /// carries its own topology and needs no shape.
    std::string_view architecture{};
    std::string_view model_version{};
    std::string_view artifact_id{};
    /// Lower- or upper-case hex SHA-256 of the artifact.
    std::string_view artifact_sha256{};
    std::string_view license{};
    std::string_view runtime{};
    std::uint64_t artifact_size_bytes = 0;
    std::uint32_t sample_rate_hz = 0;
    /// A shipped model must be redistributable unless built for local
    /// research.
    bool redistributable = false;
    bool local_research = false;
};

/// The family's state shape: one layer per family (a single scalar TCN
/// layer, one LSTM/GRU layer, one diagonal SSM). Deeper models fit through a
/// kernel that owns its topology. Only the fields for the requested family
/// are read; WaveNet, Custom and "nam.tcn" kernels own their topology.
struct GpuNeuralModelShape {
    /// Tcn: one scalar dilated causal layer.
    std::uint32_t tcn_kernel_size = 0;
    std::uint32_t tcn_dilation = 0;
    /// Lstm, Gru: one scalar-input layer.
    std::uint32_t recurrent_hidden_size = 0;
    /// Ssm: a diagonal state space.
    std::uint32_t ssm_state_size = 0;
};

struct GpuNeuralModelRequest {
    NeuralFamily family = NeuralFamily::Unknown;
    GpuNeuralModelManifest manifest{};
    GpuNeuralModelShape shape{};
    std::uint32_t block_size = 0;
    /// One kernel per audio channel.
    std::span<const GpuNeuralCpuKernel> channel_kernels{};
    /// WaveNet only: run the GPU path with these session settings (copied).
    /// Null builds a CPU-only model. A provider that cannot be prepared
    /// leaves the node on its CPU model, reported Degraded.
    const GpuWaveNetSession::Config* wavenet_gpu = nullptr;
    /// GPU blocks in flight; at most the session's slots. A node using this
    /// model may not set a larger max_inflight.
    std::uint32_t gpu_max_inflight = 1;
};

enum class GpuNeuralModelError : std::uint8_t {
    None,
    UnsupportedFamily,
    InvalidManifest,
    InvalidBlockSize,
    MissingKernels,
    ShapeMismatch,
    /// A GPU session was requested for a family without a GPU kernel.
    UnsupportedBackend,
    AllocationFailed,
};

struct GpuNeuralModelResult {
    GpuNeuralModelHandle model;
    GpuNeuralModelError error = GpuNeuralModelError::None;
    explicit operator bool() const noexcept {
        return model != nullptr && error == GpuNeuralModelError::None;
    }
};

/// Host thread only. Validates the manifest, derives the family's state
/// schema from its shape, and builds a model for one GpuNeuralRealtimeNode.
/// Never throws; a failure is reported in the result.
GpuNeuralModelResult make_gpu_neural_model(const GpuNeuralModelRequest& request) noexcept;

} // namespace pulp::gpu_audio
