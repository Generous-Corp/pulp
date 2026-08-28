#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace pulp::tooling::gpu_health {
struct HealthReadResult;
}

namespace pulp::inspect {

/// UI-thread producer for the immutable snapshot consumed by
/// `dev.pulp.gpu/health.read@1`. Recording methods fail closed when called from
/// another thread; snapshot() is lock-free, bounded, and safe for a control
/// worker. No method is suitable for an audio callback.
class ControlGpuHealthProvider final {
  public:
    enum class CacheState { cold, warm };

    struct Config {
        std::string pulp_build_id;
        std::string budget_id{"pulp.editor-first-visible.v1"};
        std::uint32_t budget_version = 1;
        std::uint32_t event_capacity = 64;
        std::chrono::milliseconds timeout{5000};
        bool seed_blank_first_frame = false;
    };

    struct AdapterIdentity {
        bool available = false;
        bool native_bridge = false;
        std::string backend;
        std::string name;
        std::string vendor;
        std::string architecture;
    };

    struct FrameObservation {
        AdapterIdentity adapter;
        bool capture_valid = false;
        bool content_floor_passed = false;
        std::uint64_t non_transparent_pixel_count = 0;
        std::uint64_t distinct_color_count = 0;
        std::string observed_signature_sha256;
        std::chrono::steady_clock::time_point observed_at;
    };

    explicit ControlGpuHealthProvider(Config config);
    ~ControlGpuHealthProvider();
    ControlGpuHealthProvider(const ControlGpuHealthProvider&) = delete;
    ControlGpuHealthProvider& operator=(const ControlGpuHealthProvider&) = delete;

    /// Arms one bounded editor-open trial. Only one trial may be in flight.
    bool begin_editor_open(CacheState cache_state,
                           std::chrono::steady_clock::time_point requested_at) noexcept;
    bool record_presented_frame(const FrameObservation& frame) noexcept;
    bool record_timeout(std::chrono::steady_clock::time_point observed_at) noexcept;
    bool record_instance_lost() noexcept;
    bool record_dropped_events(std::uint64_t count) noexcept;
    bool awaiting_frame() const noexcept;

    std::shared_ptr<const tooling::gpu_health::HealthReadResult> snapshot() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
