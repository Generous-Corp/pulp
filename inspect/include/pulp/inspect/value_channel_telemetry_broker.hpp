#pragma once

#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/session.hpp>
#include <pulp/view/value_channel_telemetry.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace pulp::inspect {

/// Transport-independent reader and bounded fan-out owner for value-channel
/// telemetry. All methods are called serially on one control thread; the
/// attachment remains the sole reader of its telemetry sidecars.
class ValueChannelTelemetryBroker {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using EventSink = std::function<InspectorTargetedEventResult(
        std::string_view client_id, const InspectorMessage& event, std::string_view loss_owner)>;
    using EventRetirementSink = std::function<void(
        std::string_view client_id, std::string_view loss_owner)>;

    struct Config {
        double default_rate_hz = 15.0;
        double max_rate_hz = 60.0;
        std::size_t max_channels = 32;
        std::size_t max_vector_values = 512;
        std::size_t max_events_per_channel = 128;
        std::size_t max_wire_bytes = 64u * 1024u;
        std::chrono::milliseconds stale_after = std::chrono::milliseconds(500);
    };

    ValueChannelTelemetryBroker();
    explicit ValueChannelTelemetryBroker(Config config, Clock clock = {});
    ~ValueChannelTelemetryBroker();
    ValueChannelTelemetryBroker(ValueChannelTelemetryBroker&&) noexcept;
    ValueChannelTelemetryBroker& operator=(ValueChannelTelemetryBroker&&) noexcept;
    ValueChannelTelemetryBroker(const ValueChannelTelemetryBroker&) = delete;
    ValueChannelTelemetryBroker& operator=(const ValueChannelTelemetryBroker&) = delete;

    void set_event_sink(EventSink sink);
    void set_event_retirement_sink(EventRetirementSink sink);

    /// Replace the sole-reader attachment. An invalid attachment explicitly
    /// clears the prior source and returns false. Subscriptions retain their
    /// channel names and are resolved again against each valid generation.
    bool replace_attachment(view::ValueChannelTelemetryAttachment attachment);
    /// Record a source generation whose catalog is intentionally empty without
    /// retaining a reader attachment. Existing subscriptions observe the
    /// generation transition and resolve their names as unavailable.
    void replace_with_empty_source();
    void clear_attachment();

    /// Handle Telemetry.getSnapshot, Telemetry.subscribe, or
    /// Telemetry.unsubscribe. Authenticated identity comes only from context.
    InspectorMessage handle(const InspectorRequestContext& context,
                            const InspectorMessage& request);

    /// Drain the attachment once and emit every due subscription.
    void poll();
    void disconnect(std::string_view client_id);
    std::size_t subscription_count() const noexcept;
    std::uint64_t source_generation() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
