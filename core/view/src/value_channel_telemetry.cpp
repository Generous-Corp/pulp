#include <pulp/view/value_channel_telemetry.hpp>

#include <atomic>
#include <chrono>
#include <utility>

#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/triple_buffer.hpp>

namespace pulp::view::detail {

namespace {

constexpr std::size_t kEventTapCapacity = 8;
std::atomic<std::uint64_t> next_value_channel_set_identity{1};

std::int64_t ui_snapshot_time_ns() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}

template <typename Payload>
struct ContinuousSnapshot {
    Payload payload{};
    std::uint64_t source_publication = 0;
    std::uint64_t ui_snapshot_sequence = 0;
    std::uint64_t attachment_generation = 0;
    std::int64_t ui_snapshot_time_ns = 0;
    bool available = false;
};

}  // namespace

class ValueChannelTelemetryControl {
public:
    const std::uint64_t generation_identity =
        next_value_channel_set_identity.fetch_add(1, std::memory_order_relaxed);
    std::atomic<bool> claimed{false};
};

class ValueChannelTelemetryState {
public:
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "value-channel telemetry activation must be lock-free");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "value-channel attachment generations must be lock-free");

    virtual ~ValueChannelTelemetryState() = default;
    virtual ValueChannelShape shape() const noexcept = 0;
    virtual void activate() noexcept {
        active_generation_.store(++next_generation_, std::memory_order_release);
    }
    virtual void deactivate() noexcept {
        active_generation_.store(0, std::memory_order_release);
    }
    virtual bool read_continuous(ValueChannelTelemetrySnapshot&) noexcept { return false; }
    virtual bool try_pop_events(ValueChannelTelemetryEventBatch&) noexcept { return false; }
    virtual ValueChannelTelemetryEventStats event_stats() const noexcept { return {}; }

    std::uint64_t active_generation() const noexcept {
        return active_generation_.load(std::memory_order_acquire);
    }
    bool source_alive() const noexcept { return source_alive_.load(std::memory_order_acquire); }
    void mark_source_dead() noexcept {
        source_alive_.store(false, std::memory_order_release);
    }

private:
    std::atomic<std::uint64_t> active_generation_{0};
    std::atomic<bool> source_alive_{true};
    std::uint64_t next_generation_ = 0;
};

template <typename Payload, ValueChannelShape Shape>
class ContinuousTelemetryState : public ValueChannelTelemetryState {
public:
    ValueChannelShape shape() const noexcept override { return Shape; }

    void record(const Payload& payload, std::uint64_t source_publication) noexcept {
        const auto generation = active_generation();
        if (generation == 0)
            return;
        ContinuousSnapshot<Payload> snapshot;
        snapshot.payload = payload;
        snapshot.source_publication = source_publication;
        snapshot.ui_snapshot_sequence = ++ui_snapshot_sequence_;
        snapshot.attachment_generation = generation;
        snapshot.ui_snapshot_time_ns = ui_snapshot_time_ns();
        snapshot.available = true;
        snapshots_.write(snapshot);
    }

    bool read_continuous(ValueChannelTelemetrySnapshot& result) noexcept override {
        const auto snapshot = snapshots_.read();
        result.shape = Shape;
        result.payload = snapshot.payload;
        result.source_publication = snapshot.source_publication;
        result.ui_snapshot_sequence = snapshot.ui_snapshot_sequence;
        result.ui_snapshot_time_ns = snapshot.ui_snapshot_time_ns;
        result.available = snapshot.available &&
                           snapshot.attachment_generation == active_generation();
        return true;
    }

private:
    runtime::TripleBuffer<ContinuousSnapshot<Payload>> snapshots_;
    std::uint64_t ui_snapshot_sequence_ = 0;
};

class ScalarTelemetryState final
    : public ContinuousTelemetryState<float, ValueChannelShape::scalar> {};
class MeterTelemetryState final
    : public ContinuousTelemetryState<MeterFrame, ValueChannelShape::meter> {};
class VectorTelemetryState final
    : public ContinuousTelemetryState<VectorFrame, ValueChannelShape::vector> {};

class EventTelemetryState final : public ValueChannelTelemetryState {
public:
    ValueChannelShape shape() const noexcept override {
        return ValueChannelShape::events;
    }

    void activate() noexcept override {
        QueuedEventBatch discarded;
        while (events_.try_pop(discarded)) {
        }
        ValueChannelTelemetryState::activate();
    }

    void push(const EventFrame& frame, std::uint64_t source_publication) noexcept {
        const auto generation = active_generation();
        if (generation == 0)
            return;
        QueuedEventBatch batch;
        batch.value.frame = frame;
        batch.value.source_publication = source_publication;
        batch.attachment_generation = generation;
        events_.try_push(batch);
    }

    bool try_pop_events(ValueChannelTelemetryEventBatch& batch) noexcept override {
        QueuedEventBatch queued;
        const auto generation = active_generation();
        while (events_.try_pop(queued)) {
            if (generation != 0 && queued.attachment_generation == generation) {
                batch = queued.value;
                return true;
            }
        }
        return false;
    }

    ValueChannelTelemetryEventStats event_stats() const noexcept override {
        const auto stats = events_.telemetry();
        return {
            .size_approx = stats.size_approx,
            .capacity = stats.capacity,
            .overflow_count = stats.overflow_count,
        };
    }

private:
    struct QueuedEventBatch {
        ValueChannelTelemetryEventBatch value;
        std::uint64_t attachment_generation = 0;
    };

    runtime::SpscQueue<QueuedEventBatch, kEventTapCapacity> events_;
};

std::shared_ptr<ValueChannelTelemetryControl> make_value_channel_telemetry_control() {
    return std::make_shared<ValueChannelTelemetryControl>();
}

std::uint64_t value_channel_telemetry_control_identity(
    const ValueChannelTelemetryControl* control) noexcept {
    return control != nullptr ? control->generation_identity : 0;
}

std::shared_ptr<ValueChannelTelemetryState> make_scalar_telemetry_state() {
    return std::make_shared<ScalarTelemetryState>();
}

std::shared_ptr<ValueChannelTelemetryState> make_meter_telemetry_state() {
    return std::make_shared<MeterTelemetryState>();
}

std::shared_ptr<ValueChannelTelemetryState> make_vector_telemetry_state() {
    return std::make_shared<VectorTelemetryState>();
}

std::shared_ptr<ValueChannelTelemetryState> make_event_telemetry_state() {
    return std::make_shared<EventTelemetryState>();
}

ScalarTelemetryState* scalar_telemetry_writer(ValueChannelTelemetryState* state) noexcept {
    return static_cast<ScalarTelemetryState*>(state);
}

MeterTelemetryState* meter_telemetry_writer(ValueChannelTelemetryState* state) noexcept {
    return static_cast<MeterTelemetryState*>(state);
}

VectorTelemetryState* vector_telemetry_writer(ValueChannelTelemetryState* state) noexcept {
    return static_cast<VectorTelemetryState*>(state);
}

EventTelemetryState* event_telemetry_writer(ValueChannelTelemetryState* state) noexcept {
    return static_cast<EventTelemetryState*>(state);
}

void record_scalar_snapshot(ScalarTelemetryState* state, float value,
                            std::uint64_t publication) noexcept {
    if (state)
        state->record(value, publication);
}

void record_meter_snapshot(MeterTelemetryState* state, const MeterFrame& frame,
                           std::uint64_t publication) noexcept {
    if (state)
        state->record(frame, publication);
}

void record_vector_snapshot(VectorTelemetryState* state, const VectorFrame& frame,
                            std::uint64_t publication) noexcept {
    if (state)
        state->record(frame, publication);
}

void push_event_telemetry(EventTelemetryState* state, const EventFrame& frame,
                          std::uint64_t publication) noexcept {
    if (state)
        state->push(frame, publication);
}

void mark_scalar_source_dead(ScalarTelemetryState* state) noexcept {
    if (state)
        state->mark_source_dead();
}

void mark_meter_source_dead(MeterTelemetryState* state) noexcept {
    if (state)
        state->mark_source_dead();
}

void mark_vector_source_dead(VectorTelemetryState* state) noexcept {
    if (state)
        state->mark_source_dead();
}

void mark_event_source_dead(EventTelemetryState* state) noexcept {
    if (state)
        state->mark_source_dead();
}

}  // namespace pulp::view::detail

namespace pulp::view {

struct ValueChannelTelemetryAttachment::Impl {
    Impl(std::shared_ptr<detail::ValueChannelTelemetryControl> control_in,
         std::vector<std::shared_ptr<detail::ValueChannelTelemetryState>> states_in,
         std::vector<ValueChannelInfo> infos)
        : control(std::move(control_in)), states(std::move(states_in)) {
        if (!control || states.size() != infos.size())
            return;
        descriptors.reserve(infos.size());
        for (std::size_t i = 0; i < infos.size(); ++i) {
            descriptors.push_back({
                .index = i,
                .info = std::move(infos[i]),
                .has_source_timestamp = false,
                .has_ui_snapshot_timestamp =
                    states[i]->shape() != ValueChannelShape::events,
            });
        }
        bool expected = false;
        if (!control->claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
        for (const auto& state : states) {
            state->activate();
        }
        claimed = true;
    }

    ~Impl() {
        if (!claimed)
            return;
        for (const auto& state : states)
            state->deactivate();
        control->claimed.store(false, std::memory_order_release);
    }

    std::shared_ptr<detail::ValueChannelTelemetryControl> control;
    std::vector<std::shared_ptr<detail::ValueChannelTelemetryState>> states;
    std::vector<ValueChannelTelemetryDescriptor> descriptors;
    bool claimed = false;
};

ValueChannelTelemetryAttachment::ValueChannelTelemetryAttachment(
    std::shared_ptr<detail::ValueChannelTelemetryControl> control,
    std::vector<std::shared_ptr<detail::ValueChannelTelemetryState>> states,
    std::vector<ValueChannelInfo> infos)
    : impl_(std::make_unique<Impl>(std::move(control), std::move(states),
                                   std::move(infos))) {
    if (!impl_->claimed)
        impl_.reset();
}

ValueChannelTelemetryAttachment::ValueChannelTelemetryAttachment() = default;
ValueChannelTelemetryAttachment::~ValueChannelTelemetryAttachment() = default;
ValueChannelTelemetryAttachment::ValueChannelTelemetryAttachment(
    ValueChannelTelemetryAttachment&&) noexcept = default;
ValueChannelTelemetryAttachment& ValueChannelTelemetryAttachment::operator=(
    ValueChannelTelemetryAttachment&&) noexcept = default;

bool ValueChannelTelemetryAttachment::valid() const noexcept {
    return impl_ != nullptr;
}

std::span<const ValueChannelTelemetryDescriptor>
ValueChannelTelemetryAttachment::channels() const noexcept {
    if (!impl_)
        return {};
    return impl_->descriptors;
}

bool ValueChannelTelemetryAttachment::read_continuous(
    std::size_t index, ValueChannelTelemetrySnapshot& snapshot) noexcept {
    return impl_ && index < impl_->states.size() &&
           impl_->states[index]->read_continuous(snapshot);
}

bool ValueChannelTelemetryAttachment::try_pop_events(
    std::size_t index, ValueChannelTelemetryEventBatch& batch) noexcept {
    return impl_ && index < impl_->states.size() &&
           impl_->states[index]->try_pop_events(batch);
}

ValueChannelTelemetryEventStats ValueChannelTelemetryAttachment::event_stats(
    std::size_t index) const noexcept {
    if (!impl_ || index >= impl_->states.size())
        return {};
    return impl_->states[index]->event_stats();
}

bool ValueChannelTelemetryAttachment::source_alive(std::size_t index) const noexcept {
    return impl_ && index < impl_->states.size() &&
           impl_->states[index]->source_alive();
}

}  // namespace pulp::view
