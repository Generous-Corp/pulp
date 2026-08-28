#pragma once

#include <pulp/inspect/control_gpu_health_provider.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulp::inspect {

/// Pulp-owned UI-thread bridge from a concrete Standalone/plugin/constrained
/// host to ControlGpuHealthProvider. The callbacks also make the same adapter
/// usable by headless and Forge canaries without depending on a native window
/// implementation. Capture completion bounds back-buffer readiness but is not
/// a first-visible/present endpoint; exact present and trace identities remain
/// unverified until supplied by the native render lifecycle.
class ControlGpuHealthViewAdapter final {
  public:
    struct Config {
        std::shared_ptr<ControlGpuHealthProvider> provider;
        std::function<std::vector<std::uint8_t>()> capture_back_buffer_png;
        /// Returns optional native lifecycle evidence without making this
        /// Pulp-owned adapter depend on the generic GPU surface implementation.
        /// A capture-only host may return adapter identity alone; exact product
        /// adapters can additionally prove submission, native present with an
        /// independently sourced presentation timestamp, lifecycle/cache
        /// identity, hitch, stage timing, and same-instance trace correlation.
        std::function<ControlGpuHealthProvider::FrameObservation()> frame_evidence;
        /// Samples the observation boundary immediately after capture/readback
        /// returns, before PNG analysis. Tests may inject a deterministic
        /// monotonic clock.
        std::function<std::chrono::steady_clock::time_point()> capture_completed_at;
    };

    static std::unique_ptr<ControlGpuHealthViewAdapter> create(Config config);
    ~ControlGpuHealthViewAdapter();
    ControlGpuHealthViewAdapter(const ControlGpuHealthViewAdapter&) = delete;
    ControlGpuHealthViewAdapter& operator=(const ControlGpuHealthViewAdapter&) = delete;

    /// Poll from the host UI thread after its ordinary render/event-loop work.
    void poll(std::chrono::steady_clock::time_point now) noexcept;

  private:
    explicit ControlGpuHealthViewAdapter(Config config);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
