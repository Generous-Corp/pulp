#pragma once

#include <pulp/format/offline_render_host.hpp>

#include <cstdint>
#include <vector>

namespace pulp::format {

/// Offline/control-thread bounce policy for turning a rendered processor span
/// into sampler-owned audio. Zero budget values mean unlimited.
struct OfflineSampleBouncePolicy {
    std::uint32_t max_channels = 0;
    std::uint64_t max_frames = 0;
    std::uint64_t max_decoded_bytes = 0;
    std::vector<std::uint32_t> allowed_sample_rates;
};

enum class OfflineSampleBounceStatus : std::uint8_t {
    ok,
    render_failed,
    empty_render,
    invalid_sample_rate,
    channel_budget_exceeded,
    frame_budget_exceeded,
    byte_budget_exceeded,
    sample_rate_not_allowed,
    bank_unprepared,
    publish_failed,
};

[[nodiscard]] const char* offline_sample_bounce_status_name(
    OfflineSampleBounceStatus status) noexcept;

struct OfflineSampleBounceRequest {
    OfflineRenderOptions render_options;
    OfflineSampleBouncePolicy policy;
    std::uint64_t audio_safe_generation = 0;
};

struct OfflineSampleBounceResult {
    OfflineSampleBounceStatus status = OfflineSampleBounceStatus::render_failed;
    OfflineRenderResult render;
    double sample_rate = 0.0;
    std::uint64_t decoded_bytes = 0;

    [[nodiscard]] bool ok() const noexcept {
        return status == OfflineSampleBounceStatus::ok;
    }
};

/// Control/background-thread helper over OfflineRenderHost. Rendering may
/// allocate and call arbitrary processor code; never call this from an audio
/// callback.
class OfflineSampleBounce {
public:
    [[nodiscard]] static OfflineSampleBounceResult render_to_buffer(
        OfflineRenderHost& host,
        const OfflineSampleBounceRequest& request);
};

} // namespace pulp::format
