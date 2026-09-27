#pragma once

#include <memory>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_wavenet.hpp>

namespace pulp::gpu_audio {
namespace detail {
struct RealtimeGpuNodePath;
struct WaveNetRealtimeTestAccess;
RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode*) noexcept;
bool requires_realtime_gpu_path(GpuAudioNode*) noexcept;
GpuAudioProvider realtime_gpu_provider(GpuAudioNode*) noexcept;
} // namespace detail

/// Experimental stamped WaveNet bridge. Provider work runs only on the transport
/// worker; callbacks never encode, submit, wait, or call Dawn. An application may
/// override prime_fallback/process_cpu_fallback to maintain its exact CPU shadow.
/// This removes duplicate worker inference, not the cost of that CPU shadow.
class GpuWaveNetRealtimeNode : public GpuAudioNode {
  public:
    struct Config {
        GpuWaveNetSession::Config session;
        std::uint32_t channels = 1;
        std::uint32_t lead_blocks = 2;
        std::uint32_t capacity = 8;
        std::uint32_t prewarm_blocks = 0;
        /// Optional bounded completion-service budget for each non-RT worker
        /// pump. Zero preserves nonblocking service. This is not an audio
        /// deadline and is recomputed for every pump.
        std::uint64_t completion_service_wait_ns = 0;
        MissPolicy miss_policy = MissPolicy::Silence;
        bool supports_cpu_fallback = false;
    };
    /// Copies all descriptors, dilation arrays and weights. Host thread only.
    explicit GpuWaveNetRealtimeNode(const Config&);
    ~GpuWaveNetRealtimeNode() override;
    GpuWaveNetRealtimeNode(const GpuWaveNetRealtimeNode&) = delete;
    GpuWaveNetRealtimeNode& operator=(const GpuWaveNetRealtimeNode&) = delete;

    GpuAudioNodeDescriptor descriptor() const override;
    bool prepare() override;
    /// Stop/join callback and worker first. False retains provider owners for retry.
    bool release() noexcept;
    /// Diagnostic snapshot: true before preparation or after delivery was fenced.
    /// This is not a completion count or a guarantee about the next callback.
    bool fenced() const noexcept;
    /// Not a legacy FIFO node. GpuAudioTransport rejects an unprepared instance.
    void process_block(const audio::BufferView<const float>&, audio::BufferView<float>&,
                       std::uint32_t) final;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct detail::WaveNetRealtimeTestAccess;
    friend detail::RealtimeGpuNodePath detail::realtime_gpu_node_path(GpuAudioNode*) noexcept;
    friend GpuAudioProvider detail::realtime_gpu_provider(GpuAudioNode*) noexcept;
    bool authenticated_provider() const noexcept;
    static std::uint8_t process(void*, const audio::BufferView<const float>&,
                                audio::BufferView<float>&, std::uint32_t, std::uint64_t, bool,
                                std::uint64_t) noexcept;
    static std::uint32_t service(void*, std::uint64_t) noexcept;
    static bool fence(void*) noexcept;
    static void delivered(void*, std::uint64_t, std::uint8_t, std::uint64_t,
                          std::uint64_t) noexcept;
    static std::uint64_t next_sequence(void*) noexcept;
    bool ready() const noexcept;
};
} // namespace pulp::gpu_audio
