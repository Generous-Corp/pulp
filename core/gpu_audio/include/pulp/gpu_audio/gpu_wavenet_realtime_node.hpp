#pragma once

#include <memory>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_status.hpp>
#include <pulp/gpu_audio/gpu_wavenet.hpp>

namespace pulp::gpu_audio {
class GpuAudioProviderPool;
namespace detail {
struct WaveNetRealtimeTestAccess;
struct SharedIoCompletionNotifier;
} // namespace detail

/// Experimental stamped WaveNet bridge. Provider work runs only on the transport
/// worker; callbacks never encode, submit, wait, or call Dawn. An application may
/// override prime_fallback/process_cpu_fallback to maintain its exact CPU shadow.
/// This removes duplicate worker inference, not the cost of that CPU shadow.
struct GpuWaveNetTraceConfig {
    bool enabled = false;
    bool capture_admissions = false;
    bool capture_callback_timing = false;
    std::uint32_t success_stride = 1;
};
#define PULP_GPU_WAVENET_TRACE_CONFIG_API 1

class GpuWaveNetRealtimeNode : public GpuAudioNode {
  public:
    struct Config {
        GpuWaveNetSession::Config session;
        std::uint32_t channels = 1;
        std::uint32_t lead_blocks = 2;
        std::uint32_t capacity = 8;
        /// Maximum number of submitted blocks retained by the serialized
        /// worker before it waits for retirement. Zero selects two when the
        /// session has at least two slots and the ring holds two blocks past
        /// the lead, else one: one block in flight cannot keep up when a
        /// block's round trip exceeds its period (64 frames on an M1 Max). The
        /// lead, and so the latency, is unchanged. Larger values require
        /// matching provider slots and a fixed preallocated pending ring.
        std::uint32_t max_inflight = 0;
        std::uint32_t prewarm_blocks = 0;
        /// Optional bounded completion-service budget for each non-RT worker
        /// pump. Zero preserves nonblocking service. This is not an audio
        /// deadline and is recomputed for every pump.
        std::uint64_t completion_service_wait_ns = 0;
        MissPolicy miss_policy = MissPolicy::Silence;
        bool supports_cpu_fallback = false;
        /// Callback blocks that may pass with work in flight and no completion
        /// before the node latches `GpuAudioRecoveryReason::ProviderUnresponsive`.
        /// Zero selects 200 ms converted to blocks at prepare() (at 48 kHz:
        /// 150 blocks of 64 frames, 75 of 128, 38 of 256), or `capacity - 1`
        /// if that is larger, and grows the input ring to hold the window and
        /// the lead, so a hang trips the watchdog before the ring saturates. A
        /// non-zero value is used as given with the configured capacity. A
        /// provider that keeps completing, however late, is never
        /// unresponsive.
        std::uint32_t watchdog_blocks = 0;
        /// Optional host-owned pool that keeps this node's providers warm
        /// across release() and prepare(). The pool must outlive every node
        /// bound to it. A borrowed provider re-fills its causal history before
        /// it delivers, as a re-acquired one does.
        GpuAudioProviderPool* provider_pool = nullptr;
    };
    /// Copies all descriptors, dilation arrays and weights. Host thread only.
    explicit GpuWaveNetRealtimeNode(const Config&);
    /// Stop/join callback and worker first; destruction releases and closes
    /// the trace generation as release() does.
    ~GpuWaveNetRealtimeNode() override;
    GpuWaveNetRealtimeNode(const GpuWaveNetRealtimeNode&) = delete;
    GpuWaveNetRealtimeNode& operator=(const GpuWaveNetRealtimeNode&) = delete;

    GpuAudioNodeDescriptor descriptor() const override;
    /// Stop/join callback and worker first: prepare() begins with release().
    bool prepare() override;
    /// Host-only, before prepare. The sole transport worker drains bounded
    /// records; quiescent release closes the generation. No callback logging.
    bool configure_trace(const GpuWaveNetTraceConfig&) noexcept;
    /// Stop/join callback and worker first. False retains provider owners for retry.
    bool release() noexcept;
    /// release() within `budget_ns` per call: a provider that cannot drain in
    /// time abandons to quarantine (its device is destroyed, its memory stays
    /// owned) and this returns false. The node keeps ownership on false, and
    /// the host must still call release() later to complete it.
    bool release_bounded(std::uint64_t budget_ns) noexcept override;
    /// Diagnostic snapshot: true before preparation or after delivery was fenced.
    /// This is not a completion count or a guarantee about the next callback.
    bool fenced() const noexcept;
    /// First latched recovery cause, if the shared-I/O path has fenced.
    GpuAudioRecoveryReason recovery_reason() const noexcept;
    /// Sequence associated with the first latched recovery cause, or UINT64_MAX.
    std::uint64_t recovery_sequence() const noexcept;
    /// Non-finite input samples (NaN, +/-Inf) replaced with zero before GPU
    /// submission since prepare(). A dilated causal stack would otherwise
    /// carry one bad sample through its whole receptive field. Counted, not
    /// latched: the status stays Ready.
    std::uint64_t sanitized_input_samples() const noexcept;
    /// Allocation-free provider-neutral lifecycle projection for hosts that
    /// own this stamped node directly.
    GpuAudioStatus status_snapshot() const noexcept override;
    bool bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept override;
    /// Not a legacy FIFO node. GpuAudioTransport rejects an unprepared instance.
    void process_block(const audio::BufferView<const float>&, audio::BufferView<float>&,
                       std::uint32_t) final;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct detail::WaveNetRealtimeTestAccess;
    bool authenticated_provider() const noexcept;
    static std::uint8_t process(void*, const audio::BufferView<const float>&,
                                audio::BufferView<float>&, std::uint32_t, std::uint64_t, bool,
                                std::uint64_t) noexcept;
    static std::uint32_t service(void*, std::uint64_t) noexcept;
    static bool fence(void*) noexcept;
    static void delivered(void*, std::uint64_t, std::uint8_t, std::uint64_t,
                          std::uint64_t) noexcept;
    static std::uint64_t next_sequence(void*) noexcept;
    static bool reprepare(void*, std::uint64_t) noexcept;
    // Host only, while no service step runs: route every channel's provider
    // completion events to the transport worker, or clear them.
    static bool set_notifier(void*, detail::SharedIoCompletionNotifier) noexcept;
    bool ready() const noexcept;
};
} // namespace pulp::gpu_audio
