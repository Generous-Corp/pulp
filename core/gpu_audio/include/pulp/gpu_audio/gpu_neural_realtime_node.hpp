#pragma once

#include <cstdint>
#include <memory>

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_status.hpp>

namespace pulp::gpu_audio {

/// Architecture family of a neural model hosted by GpuNeuralRealtimeNode.
/// The family is a descriptor field, not a class: every family runs the same
/// realtime contract, and only WaveNet currently has a GPU kernel.
enum class NeuralFamily : std::uint8_t {
    Unknown = 0,
    WaveNet = 1,
    Tcn = 2,
    Ssm = 3,
    Lstm = 4,
    Gru = 5,
    Custom = 255,
};

/// Opaque prepared neural model. It owns one causal CPU state per audio
/// channel and, for families with a GPU kernel, an optional provider backend.
/// Pulp's host-only model factory builds it from a verified model manifest;
/// a model binds to exactly one node for its lifetime.
class GpuNeuralModel;
using GpuNeuralModelHandle = std::shared_ptr<GpuNeuralModel>;

class GpuNeuralRealtimeNode;
namespace detail {
struct RealtimeGpuNodePath;
struct RealtimeGpuHealth;
struct NeuralRealtimeTestAccess;
RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode*) noexcept;
bool requires_realtime_gpu_path(GpuAudioNode*) noexcept;
GpuAudioProvider realtime_gpu_provider(GpuAudioNode*) noexcept;
} // namespace detail

/// Architecture-neutral realtime neural node with a built-in CPU shadow.
///
/// Every family (WaveNet/NAM, TCN/NAM-TCN, SSM, LSTM, GRU, custom) owns a
/// continuously primed CPU instance of its model. The transport feeds it on
/// every callback and it emits its result `lead_blocks` callbacks later, so
/// a missed, failed, or fenced GPU block is replaced by a latency-aligned,
/// history-continuous CPU block without waiting. `cpu_fallback_prepared()`
/// is therefore true for every prepared node, and the miss policy is always
/// `MissPolicy::CpuFallback`.
///
/// Families without a GPU kernel run CPU-only: they report
/// `GpuAudioExecutionPath::Cpu`, `GpuAudioProvider::Cpu`,
/// `gpu_kernel_available() == false`, and never
/// open a GPU delivery epoch (`epoch() == 0`). A family with a GPU kernel
/// whose provider cannot be prepared runs the same CPU-only contract and
/// reports a degraded GPU request instead of failing preparation.
///
/// The shadow's cost is paid on the audio thread for every block, including
/// blocks the GPU delivers. Measure it for the model size you ship.
///
/// Recovery is manual: any latched cause, including a single invalid
/// callback, fences GPU delivery until the host prepares the node again. The
/// shadow carries the stream meanwhile. Automatic re-acquire is future work.
///
/// Threading: prepare() and release() are host-thread operations with the
/// callback and worker stopped. The queries below read lifecycle state that
/// only those two calls change, plus counters and recovery state that are
/// atomics, so they are safe from a host or UI thread at any time except
/// concurrently with prepare() or release(). A transport-hosted node is
/// sampled by the transport worker into a sequence-guarded mirror, so the
/// UI-safe read is `GpuAudioTransport::status_snapshot()`.
class GpuNeuralRealtimeNode : public GpuAudioNode {
  public:
    /// Why the last prepare() returned false.
    enum class PrepareError : std::uint8_t {
        None,
        InvalidConfig,
        FamilyMismatch,
        ChannelMismatch,
        /// max_inflight exceeds the backend's own in-flight capacity.
        BackendInflightExceeded,
        /// The model is bound to another node.
        ModelBound,
        ShadowPrepareFailed,
        /// The shadow processors were prepared but not published.
        ShadowUnpublished,
        BridgeFailed,
        ReleaseFailed,
    };

    struct Config {
        /// Must match the family the model was built for.
        NeuralFamily family = NeuralFamily::Unknown;
        GpuNeuralModelHandle model;
        /// Must match the model's channel count.
        std::uint32_t channels = 1;
        /// Fixed output delay in blocks; the CPU shadow is delayed by the
        /// same amount, so host delay compensation is family-independent.
        std::uint32_t lead_blocks = 2;
        /// GPU ring depth. Must exceed lead_blocks.
        std::uint32_t capacity = 8;
        /// Maximum GPU blocks in flight; at most capacity - lead_blocks and
        /// at most the backend's provider slots.
        std::uint32_t max_inflight = 1;
        /// Silent blocks run through the GPU and the CPU shadow at prepare so
        /// both histories begin at the same stream position.
        std::uint32_t prewarm_blocks = 0;
        /// Optional bounded completion-service budget for each non-RT worker
        /// pump. Zero preserves nonblocking service.
        std::uint64_t completion_service_wait_ns = 0;
        /// Only CpuFallback is accepted: the shadow is built in.
        MissPolicy miss_policy = MissPolicy::CpuFallback;
    };

    /// Host thread only.
    explicit GpuNeuralRealtimeNode(Config config);
    /// Stop/join the callback and worker first; destruction releases.
    ~GpuNeuralRealtimeNode() override;
    GpuNeuralRealtimeNode(const GpuNeuralRealtimeNode&) = delete;
    GpuNeuralRealtimeNode& operator=(const GpuNeuralRealtimeNode&) = delete;

    GpuAudioNodeDescriptor descriptor() const override;
    /// Stop/join callback and worker first: prepare() begins with release().
    /// Fails unless every channel's CPU shadow is prepared and published.
    bool prepare() override;
    /// Stop/join callback and worker first. False retains provider owners
    /// for retry.
    bool release() noexcept;

    /// True once every channel's shadow processor is published.
    bool cpu_fallback_prepared() const noexcept override;
    /// RT-safe. Runs the CPU shadow on this block and stages the result due
    /// `lead_blocks` callbacks ago. Called by the transport on every block.
    void prime_fallback(const audio::BufferView<const float>& input,
                        std::uint32_t n) noexcept override;
    /// RT-safe. Emits the latency-aligned block staged by prime_fallback().
    void process_cpu_fallback(const audio::BufferView<const float>& input,
                              audio::BufferView<float>& output, std::uint32_t n) noexcept override;
    /// Not a legacy FIFO node. GpuAudioTransport rejects an unprepared instance.
    void process_block(const audio::BufferView<const float>&, audio::BufferView<float>&,
                       std::uint32_t) final;

    // Host/UI reads; see the threading note on the class.

    /// Reason for the most recent failed prepare(); None after a success.
    PrepareError last_prepare_error() const noexcept;
    /// Non-finite input samples (NaN, +/-Inf) replaced with zero since
    /// prepare(). The GPU submission and the CPU shadow both see the
    /// replacement, so they stay comparable.
    std::uint64_t sanitized_input_samples() const noexcept;
    NeuralFamily family() const noexcept;
    /// True only while a GPU backend for this family is prepared and has not
    /// been quiesced by an offline fence.
    bool gpu_kernel_available() const noexcept;
    /// Current GPU delivery epoch, or zero when no GPU epoch is open. A
    /// CPU-only node stays at zero for its whole lifetime.
    std::uint64_t epoch() const noexcept;
    /// True before preparation, or once GPU delivery was fenced. A CPU-only
    /// node has no GPU delivery to fence and reports false.
    bool fenced() const noexcept;
    /// First latched recovery cause.
    GpuAudioRecoveryReason recovery_reason() const noexcept;
    /// Sequence associated with the first latched recovery cause, or UINT64_MAX.
    std::uint64_t recovery_sequence() const noexcept;
    /// Allocation-free lifecycle projection.
    GpuAudioStatus status_snapshot() const noexcept override;
    /// Allocation-free capability projection.
    GpuAudioCapabilityReport capability_report() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct detail::NeuralRealtimeTestAccess;
    friend detail::RealtimeGpuNodePath detail::realtime_gpu_node_path(GpuAudioNode*) noexcept;
    friend GpuAudioProvider detail::realtime_gpu_provider(GpuAudioNode*) noexcept;
    bool ready() const noexcept;
    bool authenticated_provider() const noexcept;
    // Provider identity for the transport: Dawn when authenticated, Cpu when
    // prepared without a GPU backend, otherwise Unknown.
    GpuAudioProvider transport_provider() const noexcept;
    static std::uint8_t process(void*, const audio::BufferView<const float>&,
                                audio::BufferView<float>&, std::uint32_t, std::uint64_t, bool,
                                std::uint64_t) noexcept;
    static std::uint32_t service(void*, std::uint64_t) noexcept;
    static bool fence(void*) noexcept;
    static void delivered(void*, std::uint64_t, std::uint8_t, std::uint64_t,
                          std::uint64_t) noexcept;
    static std::uint64_t next_sequence(void*) noexcept;
    static bool health(const void*, detail::RealtimeGpuHealth&) noexcept;
};

} // namespace pulp::gpu_audio
