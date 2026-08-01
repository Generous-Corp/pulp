#include <pulp/playback/generated_event_source.hpp>

#include <pulp/runtime/spsc_ring_index.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace pulp::playback {
namespace {

template <typename Atomic>
void saturating_increment(Atomic& value, std::uint64_t amount = 1) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    for (;;) {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        const auto next = amount > maximum - current ? maximum : current + amount;
        if (value.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                        std::memory_order_relaxed))
            return;
    }
}

template <typename Atomic>
void saturating_increment_single_writer(Atomic& value, std::uint64_t amount = 1) noexcept {
    const auto current = value.load(std::memory_order_relaxed);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    value.store(amount > maximum - current ? maximum : current + amount, std::memory_order_relaxed);
}

std::uint64_t tick_width(const GeneratedEventSpan& span) noexcept {
    return static_cast<std::uint64_t>(span.end.position.value) -
           static_cast<std::uint64_t>(span.start.position.value);
}

int compare_start(const GeneratedEventSpan& lhs, const GeneratedEventSpan& rhs) noexcept {
    if (lhs.playback_epoch != rhs.playback_epoch)
        return lhs.playback_epoch < rhs.playback_epoch ? -1 : 1;
    if (lhs.start.position.value != rhs.start.position.value)
        return lhs.start.position.value < rhs.start.position.value ? -1 : 1;
    return 0;
}

bool exact_span(const GeneratedEventSpan& lhs, const GeneratedEventSpan& rhs) noexcept {
    return lhs == rhs;
}

bool before_requested(const GeneratedEventSpan& slot,
                      const GeneratedEventSpan& requested) noexcept {
    if (slot.playback_epoch != requested.playback_epoch)
        return slot.playback_epoch < requested.playback_epoch;
    return slot.end.position.value <= requested.start.position.value;
}

bool cannot_serve_future_request(const GeneratedEventSpan& slot,
                                 const GeneratedEventSpan& requested) noexcept {
    if (slot.playback_epoch != requested.playback_epoch)
        return slot.playback_epoch < requested.playback_epoch;
    return slot.start.position.value < requested.end.position.value;
}

std::int64_t normalized_remainder(std::int64_t value, std::int64_t divisor) noexcept {
    const auto remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

} // namespace

GeneratedEventSource::GeneratedEventSource() = default;
GeneratedEventSource::~GeneratedEventSource() = default;

bool GeneratedEventSource::prepare(const GeneratedEventSourceConfig& config) {
    release();
    if (config.committed_batch_capacity == 0 || config.staged_batch_capacity == 0 ||
        config.maximum_events_per_batch == 0 || config.commit_quantize.grid.value <= 0)
        return false;
    if (config.committed_batch_capacity >=
        static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const auto committed_storage_capacity = config.committed_batch_capacity + 1;
    if (config.staged_batch_capacity > std::vector<BatchSlot>{}.max_size() ||
        committed_storage_capacity > std::vector<BatchSlot>{}.max_size() ||
        config.maximum_events_per_batch > std::vector<GeneratedEvent>{}.max_size() ||
        config.staged_batch_capacity >
            std::numeric_limits<std::size_t>::max() / config.maximum_events_per_batch ||
        committed_storage_capacity >
            std::numeric_limits<std::size_t>::max() / config.maximum_events_per_batch)
        return false;
    const auto staged_event_capacity =
        config.staged_batch_capacity * config.maximum_events_per_batch;
    const auto committed_event_capacity =
        committed_storage_capacity * config.maximum_events_per_batch;
    if (staged_event_capacity > std::vector<GeneratedEvent>{}.max_size() ||
        committed_event_capacity > std::vector<GeneratedEvent>{}.max_size())
        return false;

    const auto ladder = config.degradation.active();
    if (ladder.empty() || ladder.back() != GeneratedEventDegradation::Silence)
        return false;
    for (std::size_t index = 0; index < ladder.size(); ++index) {
        if (ladder[index] == GeneratedEventDegradation::None)
            return false;
        for (std::size_t other = index + 1; other < ladder.size(); ++other)
            if (ladder[index] == ladder[other])
                return false;
    }

#if defined(__cpp_exceptions)
    try {
#endif
        std::vector<BatchSlot> staged(config.staged_batch_capacity);
        std::vector<GeneratedEvent> staged_events(staged_event_capacity);
        std::vector<BatchSlot> committed(committed_storage_capacity);
        std::vector<GeneratedEvent> committed_events(committed_event_capacity);
        auto committed_ring =
            std::make_unique<runtime::SpscRingIndex>(static_cast<int>(committed_storage_capacity));
        config_ = config;
        staged_.swap(staged);
        staged_events_.swap(staged_events);
        committed_.swap(committed);
        committed_events_.swap(committed_events);
        committed_ring_ = std::move(committed_ring);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
#endif
    prepared_ = true;
    return true;
}

bool GeneratedEventSource::begin_playback_epoch(std::uint64_t playback_epoch) noexcept {
    if (!prepared_ || playback_epoch == 0 || (has_active_epoch_ && playback_epoch <= active_epoch_))
        return false;
    committed_ring_->reset();
    staged_count_ = 0;
    committed_epoch_ = playback_epoch;
    committed_through_ = {};
    active_epoch_ = playback_epoch;
    has_committed_frontier_ = false;
    has_active_epoch_ = true;
    consumer_elapsed_epoch_ = 0;
    consumer_elapsed_tick_ = 0;
    has_consumer_elapsed_frontier_ = false;
    elapsed_frontier_.write({});
    consecutive_deadline_misses_.store(0, std::memory_order_relaxed);
    current_degradation_index_.store(
        static_cast<std::uint8_t>(GeneratedEventDegradationLadder::maximum_steps),
        std::memory_order_relaxed);
    return true;
}

void GeneratedEventSource::release() noexcept {
    committed_ring_.reset();
    committed_events_.clear();
    committed_.clear();
    staged_events_.clear();
    staged_.clear();
    config_ = {};
    staged_count_ = 0;
    committed_epoch_ = 0;
    committed_through_ = {};
    last_commit_generation_ = 0;
    active_epoch_ = 0;
    has_committed_frontier_ = false;
    has_active_epoch_ = false;
    prepared_ = false;
    consumer_elapsed_epoch_ = 0;
    consumer_elapsed_tick_ = 0;
    has_consumer_elapsed_frontier_ = false;
    consecutive_deadline_misses_.store(0, std::memory_order_relaxed);

    elapsed_frontier_.write({});
    current_degradation_index_.store(
        static_cast<std::uint8_t>(GeneratedEventDegradationLadder::maximum_steps),
        std::memory_order_relaxed);
    committed_batches_.store(0, std::memory_order_relaxed);
    consumed_batches_.store(0, std::memory_order_relaxed);
    starvation_events_.store(0, std::memory_order_relaxed);
    lagged_ticks_.store(0, std::memory_order_relaxed);
    late_batches_.store(0, std::memory_order_relaxed);
    output_overflows_.store(0, std::memory_order_relaxed);
    deadline_misses_.store(0, std::memory_order_relaxed);
    for (auto& count : degradation_uses_)
        count.store(0, std::memory_order_relaxed);
}

bool GeneratedEventSource::valid_span(const GeneratedEventSpan& span) const noexcept {
    return span.start.position.value < span.end.position.value;
}

bool GeneratedEventSource::is_quantized(timebase::MonotonicBeat boundary) const noexcept {
    const auto grid = config_.commit_quantize.grid.value;
    if (grid <= 0)
        return false;
    return normalized_remainder(boundary.position.value, grid) ==
           normalized_remainder(config_.commit_quantize.phase.value, grid);
}

bool GeneratedEventSource::valid_events(const GeneratedEventSpan& span,
                                        std::span<const GeneratedEvent> events) const noexcept {
    const auto width = tick_width(span);
    std::int64_t previous_offset = 0;
    bool has_previous = false;
    for (const auto& event : events) {
        if (event.offset.value < 0 || static_cast<std::uint64_t>(event.offset.value) >= width)
            return false;
        if (has_previous && event.offset.value < previous_offset)
            return false;
        const auto message_type = static_cast<std::uint8_t>(event.packet.words[0] >> 28);
        if (event.packet.word_count != midi::ump_words_for_message_type(message_type))
            return false;
        previous_offset = event.offset.value;
        has_previous = true;
    }
    return true;
}

GeneratedEvent* GeneratedEventSource::staged_event_storage(std::size_t index) noexcept {
    return staged_events_.data() + index * config_.maximum_events_per_batch;
}

GeneratedEvent* GeneratedEventSource::committed_event_storage(std::size_t index) noexcept {
    return committed_events_.data() + index * config_.maximum_events_per_batch;
}

bool GeneratedEventSource::elapsed_frontier(std::uint64_t& epoch,
                                            std::int64_t& tick) const noexcept {
    const auto snapshot = elapsed_frontier_.read();
    epoch = snapshot.epoch;
    tick = snapshot.tick;
    return snapshot.present;
}

GeneratedEventStageResult
GeneratedEventSource::stage(GeneratedEventSpan span, GeneratedEventRevision revision,
                            std::span<const GeneratedEvent> events) noexcept {
    if (!prepared_)
        return {GeneratedEventStageCode::NotPrepared};
    if (!has_active_epoch_)
        return {GeneratedEventStageCode::EpochNotStarted};
    if (span.playback_epoch != active_epoch_)
        return {GeneratedEventStageCode::WrongEpoch};
    if (!valid_span(span))
        return {GeneratedEventStageCode::InvalidSpan};
    if (!is_quantized(span.start) || !is_quantized(span.end))
        return {GeneratedEventStageCode::NotQuantized};
    if (revision == 0)
        return {GeneratedEventStageCode::InvalidRevision};
    if (events.size() > config_.maximum_events_per_batch)
        return {GeneratedEventStageCode::EventCapacityExceeded};
    if (!valid_events(span, events))
        return {GeneratedEventStageCode::InvalidEvent};

    if (has_committed_frontier_ &&
        (span.playback_epoch < committed_epoch_ ||
         (span.playback_epoch == committed_epoch_ &&
          span.start.position.value < committed_through_.position.value)))
        return {GeneratedEventStageCode::FrozenSpan};

    std::uint64_t elapsed_epoch = 0;
    std::int64_t elapsed_tick = 0;
    if (elapsed_frontier(elapsed_epoch, elapsed_tick) &&
        (span.playback_epoch < elapsed_epoch ||
         (span.playback_epoch == elapsed_epoch && span.start.position.value < elapsed_tick)))
        return {GeneratedEventStageCode::FrozenSpan};

    for (std::size_t index = 0; index < staged_count_; ++index) {
        auto& existing = staged_[index];
        if (!exact_span(existing.span, span))
            continue;
        if (revision <= existing.revision)
            return {GeneratedEventStageCode::StaleRevision};
        std::copy(events.begin(), events.end(), staged_event_storage(index));
        existing.revision = revision;
        existing.event_count = events.size();
        return {GeneratedEventStageCode::Revised};
    }

    if (staged_count_ != 0) {
        const auto& previous = staged_[staged_count_ - 1].span;
        if (compare_start(span, previous) < 0)
            return {GeneratedEventStageCode::OutOfOrder};
        if (span.playback_epoch == previous.playback_epoch &&
            span.start.position.value < previous.end.position.value)
            return {GeneratedEventStageCode::OverlappingSpan};
    }
    if (staged_count_ == config_.staged_batch_capacity)
        return {GeneratedEventStageCode::StagingCapacityExceeded};

    auto& destination = staged_[staged_count_];
    destination.span = span;
    destination.revision = revision;
    destination.commit_generation = 0;
    destination.event_count = events.size();
    std::copy(events.begin(), events.end(), staged_event_storage(staged_count_));
    ++staged_count_;
    return {GeneratedEventStageCode::Staged};
}

GeneratedEventCommitResult
GeneratedEventSource::commit_through(std::uint64_t playback_epoch, timebase::MonotonicBeat boundary,
                                     GeneratedEventCommitGeneration generation) noexcept {
    if (!prepared_)
        return {GeneratedEventCommitCode::NotPrepared, 0};
    if (!has_active_epoch_)
        return {GeneratedEventCommitCode::EpochNotStarted, 0};
    if (playback_epoch != active_epoch_)
        return {GeneratedEventCommitCode::WrongEpoch, 0};
    if (!is_quantized(boundary))
        return {GeneratedEventCommitCode::InvalidBoundary, 0};
    if (generation == 0)
        return {GeneratedEventCommitCode::InvalidGeneration, 0};
    if (generation <= last_commit_generation_)
        return {GeneratedEventCommitCode::StaleGeneration, 0};
    if (has_committed_frontier_ && (playback_epoch < committed_epoch_ ||
                                    (playback_epoch == committed_epoch_ &&
                                     boundary.position.value < committed_through_.position.value)))
        return {GeneratedEventCommitCode::InvalidBoundary, 0};

    std::uint64_t elapsed_epoch = 0;
    std::int64_t elapsed_tick = 0;
    if (elapsed_frontier(elapsed_epoch, elapsed_tick) && elapsed_epoch == playback_epoch) {
        std::size_t elapsed_prefix = 0;
        while (elapsed_prefix < staged_count_ &&
               staged_[elapsed_prefix].span.playback_epoch == playback_epoch &&
               staged_[elapsed_prefix].span.start.position.value < elapsed_tick)
            ++elapsed_prefix;
        for (std::size_t destination = 0; destination + elapsed_prefix < staged_count_;
             ++destination) {
            const auto source = destination + elapsed_prefix;
            staged_[destination] = staged_[source];
            std::copy_n(staged_event_storage(source), staged_[source].event_count,
                        staged_event_storage(destination));
        }
        staged_count_ -= elapsed_prefix;
        if (boundary.position.value < elapsed_tick)
            return {GeneratedEventCommitCode::InvalidBoundary, 0};
    }

    std::size_t due = 0;
    for (; due < staged_count_; ++due) {
        const auto& span = staged_[due].span;
        if (span.playback_epoch < playback_epoch)
            continue;
        if (span.playback_epoch > playback_epoch)
            break;
        if (span.end.position.value <= boundary.position.value)
            continue;
        if (span.start.position.value < boundary.position.value)
            return {GeneratedEventCommitCode::InvalidBoundary, 0};
        break;
    }

    if (static_cast<std::size_t>(committed_ring_->free_space()) < due)
        return {GeneratedEventCommitCode::CommitRingFull, 0};

    int start_1 = 0;
    int size_1 = 0;
    int start_2 = 0;
    int size_2 = 0;
    committed_ring_->prepare_to_write(static_cast<int>(due), start_1, size_1, start_2, size_2);
    std::size_t source_index = 0;
    const auto copy_range = [&](int start, int count) {
        for (int offset = 0; offset < count; ++offset, ++source_index) {
            const auto destination_index = static_cast<std::size_t>(start + offset);
            committed_[destination_index] = staged_[source_index];
            committed_[destination_index].commit_generation = generation;
            std::copy_n(staged_event_storage(source_index), staged_[source_index].event_count,
                        committed_event_storage(destination_index));
        }
    };
    copy_range(start_1, size_1);
    copy_range(start_2, size_2);
    committed_ring_->finish_write(static_cast<int>(due));

    for (std::size_t destination = 0; destination + due < staged_count_; ++destination) {
        const auto source = destination + due;
        staged_[destination] = staged_[source];
        std::copy_n(staged_event_storage(source), staged_[source].event_count,
                    staged_event_storage(destination));
    }
    staged_count_ -= due;
    committed_epoch_ = playback_epoch;
    committed_through_ = boundary;
    last_commit_generation_ = generation;
    has_committed_frontier_ = true;
    saturating_increment(committed_batches_, due);
    return {GeneratedEventCommitCode::Committed, due};
}

void GeneratedEventSource::publish_elapsed(const GeneratedEventSpan& span) noexcept {
    consumer_elapsed_epoch_ = span.playback_epoch;
    consumer_elapsed_tick_ = span.end.position.value;
    has_consumer_elapsed_frontier_ = true;
    elapsed_frontier_.write({span.playback_epoch, span.end.position.value, true});
}

GeneratedEventPullResult GeneratedEventSource::pull(GeneratedEventSpan requested,
                                                    std::span<GeneratedEvent> output) noexcept {
    if (!prepared_ || !has_active_epoch_ || requested.playback_epoch != active_epoch_ ||
        !valid_span(requested) || !is_quantized(requested.start) || !is_quantized(requested.end))
        return {};
    if (has_consumer_elapsed_frontier_ && requested.playback_epoch == consumer_elapsed_epoch_ &&
        requested.start.position.value < consumer_elapsed_tick_)
        return {};

    int start_1 = 0;
    int size_1 = 0;
    int start_2 = 0;
    int size_2 = 0;
    bool discarded_stale_batch = false;
    for (;;) {
        committed_ring_->prepare_to_read(1, start_1, size_1, start_2, size_2);
        if (size_1 + size_2 == 0)
            break;
        const auto index = static_cast<std::size_t>(size_1 != 0 ? start_1 : start_2);
        const auto& slot = committed_[index];
        if (!before_requested(slot.span, requested) &&
            !cannot_serve_future_request(slot.span, requested))
            break;
        if (exact_span(slot.span, requested))
            break;
        committed_ring_->finish_read(1);
        saturating_increment_single_writer(late_batches_);
        discarded_stale_batch = true;
    }

    committed_ring_->prepare_to_read(1, start_1, size_1, start_2, size_2);
    if (size_1 + size_2 != 0) {
        const auto index = static_cast<std::size_t>(size_1 != 0 ? start_1 : start_2);
        const auto& slot = committed_[index];
        if (exact_span(slot.span, requested)) {
            const auto event_count = slot.event_count;
            const auto commit_generation = slot.commit_generation;
            if (event_count > output.size()) {
                committed_ring_->finish_read(1);
                publish_elapsed(requested);
                saturating_increment_single_writer(output_overflows_);
                return {GeneratedEventPullCode::OutputOverflow, 0,   commit_generation, 0,
                        GeneratedEventDegradation::Silence,     true};
            }
            std::copy_n(committed_event_storage(index), event_count, output.begin());
            committed_ring_->finish_read(1);
            publish_elapsed(requested);
            saturating_increment_single_writer(consumed_batches_);
            return {GeneratedEventPullCode::Ready, event_count,          commit_generation, 0,
                    current_degradation(),         discarded_stale_batch};
        }
    }

    publish_elapsed(requested);
    const auto lag = tick_width(requested);
    saturating_increment_single_writer(starvation_events_);
    saturating_increment_single_writer(lagged_ticks_, lag);
    const auto selected = current_degradation();
    const auto fallback =
        selected == GeneratedEventDegradation::None ? GeneratedEventDegradation::Silence : selected;
    return {GeneratedEventPullCode::Starved, 0, 0, lag, fallback, true};
}

GeneratedEventDegradation GeneratedEventSource::record_deadline_miss() noexcept {
    const auto ladder = config_.degradation.active();
    if (ladder.empty())
        return GeneratedEventDegradation::Silence;
    auto consecutive = consecutive_deadline_misses_.load(std::memory_order_relaxed);
    for (;;) {
        const auto next = consecutive == std::numeric_limits<std::uint8_t>::max()
                              ? consecutive
                              : static_cast<std::uint8_t>(consecutive + 1);
        if (consecutive_deadline_misses_.compare_exchange_weak(
                consecutive, next, std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }
    const auto index = std::min<std::size_t>(consecutive, ladder.size() - 1);
    current_degradation_index_.store(static_cast<std::uint8_t>(index), std::memory_order_relaxed);
    saturating_increment(deadline_misses_);
    saturating_increment(degradation_uses_[index]);
    return ladder[index];
}

void GeneratedEventSource::record_deadline_met() noexcept {
    consecutive_deadline_misses_.store(0, std::memory_order_relaxed);
    current_degradation_index_.store(
        static_cast<std::uint8_t>(GeneratedEventDegradationLadder::maximum_steps),
        std::memory_order_relaxed);
}

GeneratedEventDegradation GeneratedEventSource::current_degradation() const noexcept {
    const auto ladder = config_.degradation.active();
    if (ladder.empty())
        return GeneratedEventDegradation::Silence;
    const auto selected = current_degradation_index_.load(std::memory_order_relaxed);
    if (selected >= ladder.size())
        return GeneratedEventDegradation::None;
    const auto index = static_cast<std::size_t>(selected);
    return ladder[index];
}

GeneratedEventSource::Stats GeneratedEventSource::stats() const noexcept {
    Stats result;
    result.committed_batches = committed_batches_.load(std::memory_order_relaxed);
    result.consumed_batches = consumed_batches_.load(std::memory_order_relaxed);
    result.starvation_events = starvation_events_.load(std::memory_order_relaxed);
    result.lagged_ticks = lagged_ticks_.load(std::memory_order_relaxed);
    result.late_batches = late_batches_.load(std::memory_order_relaxed);
    result.output_overflows = output_overflows_.load(std::memory_order_relaxed);
    result.deadline_misses = deadline_misses_.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < degradation_uses_.size(); ++index)
        result.degradation_uses[index] = degradation_uses_[index].load(std::memory_order_relaxed);
    if (committed_ring_)
        result.committed_batches_ready = static_cast<std::size_t>(committed_ring_->num_ready());
    return result;
}

} // namespace pulp::playback
