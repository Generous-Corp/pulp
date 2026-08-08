#pragma once

#include <pulp/view/value_channel_telemetry.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulp::inspect {

enum class ControlTelemetrySensitivity : std::uint8_t { Observable, Sensitive };

struct ControlTelemetryAuthority {
    std::string client_id;
    std::string registration_id;
    std::string instance_id;
    std::string grant_id;
    bool allow_sensitive = false;
};

struct ControlTelemetrySubscriptionRequest {
    std::vector<std::string> channels;
    double rate_hz = 15.0;
    std::size_t maximum_vector_values = 256;
};

struct ControlTelemetryChannelSample {
    std::string channel;
    view::ValueChannelShape shape = view::ValueChannelShape::scalar;
    std::vector<float> values;
    std::uint64_t source_publication = 0;
    std::uint64_t source_dropped = 0;
    bool redacted = false;
    bool source_alive = false;
};

struct ControlTelemetryFrame {
    std::uint64_t sequence = 0;
    std::uint64_t sampled_at_ns = 0;
    std::uint64_t dropped_since_previous = 0;
    std::vector<ControlTelemetryChannelSample> channels;
};

struct ControlTelemetryTapConfig {
    /// Fail closed until a developer/test host explicitly enables the tap.
    bool enabled = false;
    double minimum_rate_hz = 0.1;
    double maximum_rate_hz = 60.0;
    std::size_t maximum_subscriptions = 32;
    std::size_t maximum_subscriptions_per_client = 4;
    std::size_t maximum_channels_per_subscription = 32;
    std::size_t maximum_vector_values = 512;
    std::size_t maximum_queued_frames = 4;
};

/// Canonical transport-free, bounded fan-out telemetry tap. It owns the one
/// ValueChannelTelemetryAttachment reader and copies each sampled publication
/// into bounded per-subscription queues. Slow consumers lose oldest frames and
/// receive exact loss accounting; they can never block producer/render/audio.
/// All methods are serialized by the broker control thread; poll() performs the
/// bounded copies and allocations away from producer/render/audio threads.
class ControlTelemetryTap {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Classifier = std::function<ControlTelemetrySensitivity(std::string_view)>;

    explicit ControlTelemetryTap(ControlTelemetryTapConfig config, Clock clock = {});
    ~ControlTelemetryTap();
    ControlTelemetryTap(ControlTelemetryTap&&) noexcept;
    ControlTelemetryTap& operator=(ControlTelemetryTap&&) noexcept;
    ControlTelemetryTap(const ControlTelemetryTap&) = delete;
    ControlTelemetryTap& operator=(const ControlTelemetryTap&) = delete;

    bool attach(view::ValueChannelTelemetryAttachment attachment, Classifier classifier = {});
    void detach() noexcept;
    std::optional<std::string> subscribe(const ControlTelemetryAuthority& authority,
                                         ControlTelemetrySubscriptionRequest request);
    bool unsubscribe(std::string_view subscription_id, const ControlTelemetryAuthority& authority);
    void poll();
    std::optional<ControlTelemetryFrame> try_pop(std::string_view subscription_id,
                                                 const ControlTelemetryAuthority& authority);
    std::size_t subscription_count() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
