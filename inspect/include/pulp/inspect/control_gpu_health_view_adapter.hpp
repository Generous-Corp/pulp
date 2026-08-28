#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulp::render { class GpuSurface; }

namespace pulp::inspect {

class ControlGpuHealthProvider;

/// Pulp-owned UI-thread bridge from a concrete Standalone/plugin/constrained
/// host to ControlGpuHealthProvider. The callbacks also make the same adapter
/// usable by headless and Forge canaries without depending on a native window
/// implementation. It deliberately records a capture-confirmed upper bound;
/// exact present and trace identities remain unverified until supplied by the
/// generic render lifecycle.
class ControlGpuHealthViewAdapter final {
  public:
    struct Config {
        std::shared_ptr<ControlGpuHealthProvider> provider;
        std::function<std::vector<std::uint8_t>()> capture_back_buffer_png;
        std::function<render::GpuSurface*()> gpu_surface;
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
