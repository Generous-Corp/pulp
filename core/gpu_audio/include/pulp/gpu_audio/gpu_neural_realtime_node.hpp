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
struct RealtimeGpuHealth;
struct NeuralRealtimeTestAccess;
struct SharedIoProviderIdentity;
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
/// Where the shadow runs is the ShadowPolicy. Inline runs it on the audio
/// thread every block, including blocks the GPU delivers. Worker runs it on a
/// dedicated non-realtime thread: the callback only copies its input in and,
/// when the GPU misses, copies the shadow's stamped result out. A shadow
/// result that is late for its slot is dropped, and a block both paths miss
/// is silent. None makes a realtime GPU miss silent. Auto times one block of
/// the CPU model for one channel at prepare() and picks Inline when that cost
/// times the channel count is at most kAutoInlineMaxCostPct of the block
/// period, otherwise Worker. Families without a GPU backend, and a GPU family
/// whose provider could not be prepared, always run Inline, because the CPU
/// model is their output.
///
/// The policy governs realtime misses only. An offline render always runs the
/// CPU model inline on the same timeline: under Worker and None the model is
/// kept primed off the audio thread, the start of an offline render finishes
/// its queue and continues it inline, and the first realtime callback after
/// the render hands it back to the shadow thread. The node's `shadow_*` status fields are the
/// authoritative shadow counts; the transport's CpuFallbackDelivered label
/// means only that the fallback path ran, which can be a silent double miss.
///
/// On the GPU path the node detects device loss (the provider's loss query,
/// which outranks the failed completions it causes) and a hung provider (a
/// completion watchdog in callback blocks); both report Unavailable.
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
    /// Where the built-in CPU shadow runs.
    enum class ShadowPolicy : std::uint8_t {
        Auto = 0,
        Inline = 1,
        Worker = 2,
        /// A realtime GPU miss is silent. The model is still kept primed off
        /// the audio thread so an offline render can continue it. Requires a
        /// family with a GPU kernel.
        None = 3,
    };
    /// Auto selects Inline when the total inline cost (one channel's cost
    /// times the channel count) is at most this share of the block period.
    static constexpr float kAutoInlineMaxCostPct = 15.0f;

    /// Allocation-free shadow diagnostics.
    struct ShadowStatus {
        ShadowPolicy requested = ShadowPolicy::Auto;
        /// One block of the shadow model for one channel, as a percentage of
        /// the block period, timed on the host thread at prepare(). Negative
        /// when no shadow was prepared.
        float inline_cost_per_channel_pct = -1.0f;
        /// The per-channel cost times the channel count: what Inline would
        /// spend on the audio thread. This is what Auto compares.
        float inline_cost_total_pct = -1.0f;
        /// Worker inputs dropped because the shadow queue was full.
        std::uint64_t shadow_input_dropped = 0;
        /// Times the Worker restarted the model's state after a dropped input.
        std::uint64_t shadow_resyncs = 0;
        bool workgroup_joined = false;
    };

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
        /// ShadowPolicy::None for a family without a GPU kernel.
        ShadowRequired,
        /// The shadow model's channels, block size or sample rate differ.
        ShadowModelMismatch,
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
        ShadowPolicy shadow_policy = ShadowPolicy::Auto;
        /// Optional cheaper model for the shadow. Never a default: status
        /// reports `shadow_model_differs`, and its output will not null
        /// against the GPU model. Binds like `model`.
        GpuNeuralModelHandle shadow_model;
        /// Worker queue depth in blocks, for both directions.
        std::uint32_t shadow_queue_blocks = 16;
        /// Borrowed os_workgroup_t of the host's audio thread. The Worker
        /// shadow thread joins it; without one it runs at USER_INTERACTIVE.
        void* shadow_audio_workgroup = nullptr;
        /// GPU watchdog budget in callback blocks: a provider that completes
        /// nothing for this many blocks while work is in flight latches
        /// ProviderUnresponsive. Zero selects 200 ms converted to blocks at
        /// prepare() (at 48 kHz: 150 blocks of 64 frames, 75 of 128, 38 of
        /// 256), or `capacity - 1` if that is larger, and grows the ingress
        /// ring to hold the window and the lead. A non-zero value is used as
        /// given with the configured capacity. A provider that keeps
        /// completing, however late, is never unresponsive.
        std::uint32_t watchdog_blocks = 0;
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
    /// Invalid or out-of-sequence callbacks observed on the CPU-only path.
    /// They are counted, not latched: nothing is fenced there and the CPU
    /// model keeps delivering. On the GPU path the same violation fences GPU
    /// delivery and is reported as the recovery reason.
    std::uint64_t callback_contract_violations() const noexcept;
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
    bool bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept override;
    /// Allocation-free capability projection.
    GpuAudioCapabilityReport capability_report() const noexcept;
    /// Allocation-free shadow diagnostics beyond the public `shadow_*` fields
    /// of status_snapshot(), which carry the mode and the authoritative
    /// delivered, late and double-miss counts.
    ShadowStatus shadow_status() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct detail::NeuralRealtimeTestAccess;
    bool ready() const noexcept;
    bool authenticated_provider() const noexcept;
    // Provider identity for the transport: Dawn when authenticated, Cpu when
    // prepared without a GPU backend, otherwise Unknown.
    GpuAudioProvider transport_provider() const noexcept;
    detail::SharedIoProviderIdentity provider_identity_for_transport() const noexcept;
    static std::uint8_t process(void*, const audio::BufferView<const float>&,
                                audio::BufferView<float>&, std::uint32_t, std::uint64_t, bool,
                                std::uint64_t) noexcept;
    static std::uint32_t service(void*, std::uint64_t) noexcept;
    static bool fence(void*) noexcept;
    static void offline_render(void*, bool offline) noexcept;
    static void delivered(void*, std::uint64_t, std::uint8_t, std::uint64_t,
                          std::uint64_t) noexcept;
    static std::uint64_t next_sequence(void*) noexcept;
    static bool health(const void*, detail::RealtimeGpuHealth&) noexcept;
};

} // namespace pulp::gpu_audio
