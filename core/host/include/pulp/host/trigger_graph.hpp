#pragma once

#include <pulp/host/graph_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pulp::host {
class SignalGraph;
namespace detail {
struct TriggerState;
struct TriggerController;
struct TriggerTestAccess;
} // namespace detail

struct TriggerPayload {
    std::uint16_t token = 0;
};
enum class TriggerKind : std::uint8_t { Trigger = 1 };
struct TriggerRecord {
    std::uint64_t epoch = 0;
    std::uint64_t sample = 0;
    std::uint64_t sequence = 0;
    std::uint32_t producer = 0;
    std::uint16_t token = 0;
    std::uint8_t source_port = 0;
    TriggerKind kind = TriggerKind::Trigger;
};
static_assert(sizeof(TriggerRecord) == 32);

enum class TriggerEmitResult : std::uint8_t {
    Accepted,
    StaleEpoch,
    Unbound,
    OutOfRange,
    Future,
    Late,
    Overflow,
    AttemptLimit,
    InvalidPhase
};

// Borrowed only for one callback. Coordinates and phase belong to the graph.
class TriggerContext final {
  public:
    TriggerContext(const TriggerContext&) = delete;
    TriggerContext& operator=(const TriggerContext&) = delete;
    TriggerEmitResult emit(TriggerPayload payload) noexcept;
    TriggerEmitResult emit_at_sample(std::int64_t absolute_sample, TriggerPayload payload) noexcept;
    std::span<const TriggerRecord> received() const noexcept;

  private:
    friend struct detail::TriggerState;
    explicit TriggerContext(detail::TriggerState& state) noexcept : state_(&state) {}
    detail::TriggerState* state_;
};

struct TriggerDetectorLane {
    float threshold = 0.5f;
    std::uint16_t input_channel = 0;
    TriggerPayload payload;
};
struct TriggerProducerConfig {
    std::array<TriggerDetectorLane, 16> lanes{};
    std::uint8_t lane_count = 1;
    std::uint8_t input_channels = 1;
};
struct TriggerEnvelopeConfig {
    // Output is one on a trigger, then multiplied by decay per sample.
    float decay = 0.99f;
};
enum class TriggerAudioKind : std::uint8_t { Modulation, Lowpass };
struct TriggerAudioConfig {
    TriggerAudioKind kind = TriggerAudioKind::Modulation;
    float coefficient = 0.5f;
};
struct TriggerEdge {
    NodeId producer = 0;
    NodeId consumer = 0;
    TriggerKind kind = TriggerKind::Trigger;
    std::uint8_t source_port = 0;
    std::uint8_t destination_port = 0;
};
enum class TriggerGraphResult : std::uint8_t {
    Accepted,
    Cardinality,
    Payload,
    UnsupportedProducer,
    Feedback,
    DependencyCycle,
    Mode,
    Budget,
    Binding,
    Frames,
    ClockExhausted,
    StaleGeneration,
    Busy,
    InvalidBuffers,
    ProcessingFailure
};
struct TriggerCounters {
    std::uint64_t cursor = 0;
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint64_t overflow = 0;
    std::uint64_t diagnostic_refusals = 0;
    std::uint8_t attempts = 0;
    std::uint8_t accepted = 0;
};
struct TriggerResourceReport {
    static constexpr std::size_t limit = 2'097'152;
    std::size_t arena_bytes = 0;
    std::size_t generation_bytes = 0;
    std::size_t controller_bytes = 0;
    std::size_t descriptor_bytes = 0;
    std::size_t total() const noexcept {
        return arena_bytes + generation_bytes + controller_bytes + descriptor_bytes;
    }
};

class PreparedTriggerPlan final {
  public:
    ~PreparedTriggerPlan();
    PreparedTriggerPlan(const PreparedTriggerPlan&) = delete;
    PreparedTriggerPlan& operator=(const PreparedTriggerPlan&) = delete;

  private:
    friend class SignalGraph;
    struct Impl;
    explicit PreparedTriggerPlan(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
struct TriggerPreparation {
    TriggerGraphResult result = TriggerGraphResult::Binding;
    std::unique_ptr<PreparedTriggerPlan> plan;
};
} // namespace pulp::host
