#pragma once
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <vector>

namespace pulp::gpu_audio::detail {
// Private deterministic provider seam. No plugin-facing queue/provider hooks.
class WaveNetRealtimeChannel {
  public:
    virtual ~WaveNetRealtimeChannel() = default;
    virtual bool submit(std::span<const float>, std::uint64_t) noexcept = 0;
    virtual void service(std::uint64_t) noexcept = 0;
    virtual std::optional<GpuWaveNetBlockResult> receive(std::span<float>) noexcept = 0;
    virtual bool release() noexcept = 0;
};
struct WaveNetRealtimeTestAccess {
    static bool prepare(GpuWaveNetRealtimeNode&,
                        std::vector<std::unique_ptr<WaveNetRealtimeChannel>>,
                        std::uint64_t first_sequence = 0);
};
} // namespace pulp::gpu_audio::detail
