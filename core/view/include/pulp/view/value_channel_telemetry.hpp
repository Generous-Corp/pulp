#pragma once

/// @file value_channel_telemetry.hpp
/// Transport-free fan-out for UI-consumed value-channel snapshots and ordered
/// producer event batches.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <pulp/view/value_source.hpp>

namespace pulp::view {

/// The shape of a channel's payload.
enum class ValueChannelShape {
    scalar,  ///< one number per publish (gain reduction, envelope level)
    meter,   ///< a multi-channel `MeterFrame` (peak/RMS per channel)
    vector,  ///< a block of samples for a scope-style display
    events,  ///< a per-block list of `ValueEvent` occurrences
};

/// What a channel is, for discovery and for a UI lint to check names against.
struct ValueChannelInfo {
    std::string name;   ///< verbatim lookup key
    std::string unit;   ///< display only ("dB", "Hz"); may be empty
    ValueChannelShape shape = ValueChannelShape::scalar;
    /// The value a bound view falls back to when the channel goes stale. Event
    /// channels do not synthesize fallback occurrences, so this is ignored for
    /// their descriptors.
    float neutral = 0.0f;
};

using ValueChannelContinuousPayload =
    std::variant<float, MeterFrame, VectorFrame>;

/// A coherent continuous payload copied by the existing UI reader path.
struct ValueChannelTelemetrySnapshot {
    ValueChannelShape shape = ValueChannelShape::scalar;
    ValueChannelContinuousPayload payload = 0.0f;
    std::uint64_t source_publication = 0;
    std::uint64_t ui_snapshot_sequence = 0;
    /// Steady-clock nanoseconds captured by the UI reader. This is not the
    /// producer's publish time and must not be presented as one.
    std::int64_t ui_snapshot_time_ns = 0;
    bool available = false;
};

/// One producer event batch retained by the ordered telemetry tap.
struct ValueChannelTelemetryEventBatch {
    EventFrame frame;
    std::uint64_t source_publication = 0;
};

/// Current producer-tap state. Overflow is cumulative for the source lifetime.
struct ValueChannelTelemetryEventStats {
    std::size_t size_approx = 0;
    std::size_t capacity = 0;
    std::uint64_t overflow_count = 0;
};

/// Stable metadata for a channel held by a telemetry attachment.
struct ValueChannelTelemetryDescriptor {
    std::size_t index = 0;
    ValueChannelInfo info;
    bool has_source_timestamp = false;
    bool has_ui_snapshot_timestamp = false;
};

namespace detail {
class ValueChannelTelemetryControl;
class ValueChannelTelemetryState;
class ScalarTelemetryState;
class MeterTelemetryState;
class VectorTelemetryState;
class EventTelemetryState;
}  // namespace detail

/// Exclusive reader attachment for every telemetry sidecar in one channel set.
/// Only one attachment may exist at a time. Its continuous reads are the sole
/// reader of the sidecar buffers, and its event pops are the sole reader of the
/// producer SPSC taps. All read and pop calls on an attachment must therefore
/// be serialized on one broker thread.
class ValueChannelTelemetryAttachment {
public:
    ValueChannelTelemetryAttachment();
    ~ValueChannelTelemetryAttachment();
    ValueChannelTelemetryAttachment(ValueChannelTelemetryAttachment&&) noexcept;
    ValueChannelTelemetryAttachment& operator=(ValueChannelTelemetryAttachment&&) noexcept;
    ValueChannelTelemetryAttachment(const ValueChannelTelemetryAttachment&) = delete;
    ValueChannelTelemetryAttachment& operator=(const ValueChannelTelemetryAttachment&) = delete;

    bool valid() const noexcept;
    std::span<const ValueChannelTelemetryDescriptor> channels() const noexcept;

    /// Read the latest UI-consumed snapshot. Returns false for event channels or
    /// an invalid index. A valid continuous channel can return `available=false`
    /// until its UI path has performed a read.
    bool read_continuous(std::size_t index,
                         ValueChannelTelemetrySnapshot& snapshot) noexcept;

    /// Pop the next producer event batch. Returns false for continuous channels,
    /// an invalid index, or an empty tap.
    bool try_pop_events(std::size_t index,
                        ValueChannelTelemetryEventBatch& batch) noexcept;
    ValueChannelTelemetryEventStats event_stats(std::size_t index) const noexcept;
    bool source_alive(std::size_t index) const noexcept;

private:
    friend class ValueChannelSet;
    struct Impl;
    ValueChannelTelemetryAttachment(
        std::shared_ptr<detail::ValueChannelTelemetryControl> control,
        std::vector<std::shared_ptr<detail::ValueChannelTelemetryState>> states,
        std::vector<ValueChannelInfo> infos);
    std::unique_ptr<Impl> impl_;
};

namespace detail {

std::shared_ptr<ValueChannelTelemetryControl> make_value_channel_telemetry_control();
std::shared_ptr<ValueChannelTelemetryState> make_scalar_telemetry_state();
std::shared_ptr<ValueChannelTelemetryState> make_meter_telemetry_state();
std::shared_ptr<ValueChannelTelemetryState> make_vector_telemetry_state();
std::shared_ptr<ValueChannelTelemetryState> make_event_telemetry_state();

ScalarTelemetryState* scalar_telemetry_writer(ValueChannelTelemetryState*) noexcept;
MeterTelemetryState* meter_telemetry_writer(ValueChannelTelemetryState*) noexcept;
VectorTelemetryState* vector_telemetry_writer(ValueChannelTelemetryState*) noexcept;
EventTelemetryState* event_telemetry_writer(ValueChannelTelemetryState*) noexcept;

void record_scalar_snapshot(ScalarTelemetryState*, float, std::uint64_t) noexcept;
void record_meter_snapshot(MeterTelemetryState*, const MeterFrame&, std::uint64_t) noexcept;
void record_vector_snapshot(VectorTelemetryState*, const VectorFrame&, std::uint64_t) noexcept;
void push_event_telemetry(EventTelemetryState*, const EventFrame&, std::uint64_t) noexcept;
void mark_scalar_source_dead(ScalarTelemetryState*) noexcept;
void mark_meter_source_dead(MeterTelemetryState*) noexcept;
void mark_vector_source_dead(VectorTelemetryState*) noexcept;
void mark_event_source_dead(EventTelemetryState*) noexcept;

}  // namespace detail

}  // namespace pulp::view
