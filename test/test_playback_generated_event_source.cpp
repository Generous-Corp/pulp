#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/playback/generated_event_source.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using namespace pulp;

namespace {

constexpr auto kGrid = timebase::kTicksPerQuarter;
volatile std::size_t g_generated_event_escaping_size = 64;
float* volatile g_generated_event_escaping_block = nullptr;

playback::GeneratedEventSourceConfig config(std::size_t committed_capacity = 4,
                                            std::size_t staged_capacity = 4,
                                            std::size_t event_capacity = 4) {
    playback::GeneratedEventSourceConfig result;
    result.committed_batch_capacity = committed_capacity;
    result.staged_batch_capacity = staged_capacity;
    result.maximum_events_per_batch = event_capacity;
    result.commit_quantize.grid = {kGrid};
    result.commit_quantize.phase = {0};
    return result;
}

playback::GeneratedEventSpan span(std::int64_t start, std::int64_t end, std::uint64_t epoch = 1) {
    return {epoch, {{start}}, {{end}}};
}

playback::GeneratedEvent event(std::int64_t offset, std::uint32_t payload) {
    playback::GeneratedEvent result;
    result.offset = {offset};
    result.packet.words[0] = 0x20903c00u | (payload & 0x7fu);
    result.packet.word_count = 1;
    return result;
}

bool same_event(const playback::GeneratedEvent& lhs, const playback::GeneratedEvent& rhs) {
    return lhs.offset == rhs.offset && lhs.packet.word_count == rhs.packet.word_count &&
           lhs.packet.words == rhs.packet.words;
}

void prepare_epoch(playback::GeneratedEventSource& source,
                   const playback::GeneratedEventSourceConfig& source_config = config()) {
    REQUIRE(source.prepare(source_config));
    REQUIRE(source.begin_playback_epoch(1));
}

} // namespace

TEST_CASE("Generated event source rejects incoherent fixed-capacity contracts",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    auto invalid = config();
    invalid.committed_batch_capacity = 0;
    CHECK_FALSE(source.prepare(invalid));

    invalid = config();
    invalid.commit_quantize.grid = {0};
    CHECK_FALSE(source.prepare(invalid));

    invalid = config();
    invalid.degradation.count = 2;
    invalid.degradation.steps[0] = playback::GeneratedEventDegradation::UseSkeleton;
    invalid.degradation.steps[1] = playback::GeneratedEventDegradation::UseSkeleton;
    CHECK_FALSE(source.prepare(invalid));

    invalid.degradation.steps[1] = playback::GeneratedEventDegradation::Silence;
    REQUIRE(source.prepare(invalid));
    CHECK(source.prepared());
    CHECK_FALSE(source.begin_playback_epoch(0));
    CHECK(source.stage(span(0, kGrid), 1, std::span<const playback::GeneratedEvent>{}).code ==
          playback::GeneratedEventStageCode::EpochNotStarted);
    REQUIRE(source.begin_playback_epoch(1));

    auto oversized = config(1, 1, std::numeric_limits<std::size_t>::max() / 2 + 1);
    CHECK_FALSE(source.prepare(oversized));
    CHECK_FALSE(source.prepared());

    const auto event_max_size = std::vector<playback::GeneratedEvent>{}.max_size();
    if (event_max_size != std::numeric_limits<std::size_t>::max()) {
        oversized = config(1, 1, event_max_size + 1);
        CHECK_FALSE(source.prepare(oversized));
        CHECK_FALSE(source.prepared());
    }
}

TEST_CASE("Committed event batch is immutable and an uncommitted future is revisable",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);

    const std::array first{event(0, 10)};
    const std::array revised{event(0, 20), event(kGrid / 2, 21)};
    REQUIRE(source.stage(span(0, kGrid), 1, first).code ==
            playback::GeneratedEventStageCode::Staged);
    REQUIRE(source.stage(span(0, kGrid), 2, revised).code ==
            playback::GeneratedEventStageCode::Revised);
    CHECK(source.stage(span(0, kGrid), 2, revised).code ==
          playback::GeneratedEventStageCode::StaleRevision);
    REQUIRE(source.commit_through(1, {{kGrid}}, 1).code ==
            playback::GeneratedEventCommitCode::Committed);

    // The producer can no longer reach a published slot.
    const std::array illegal_rewrite{event(0, 99)};
    CHECK(source.stage(span(0, kGrid), 3, illegal_rewrite).code ==
          playback::GeneratedEventStageCode::FrozenSpan);

    std::array<playback::GeneratedEvent, 4> output{};
    const auto committed = source.pull(span(0, kGrid), output);
    REQUIRE(committed.code == playback::GeneratedEventPullCode::Ready);
    REQUIRE(committed.commit_generation == 1);
    REQUIRE(committed.event_count == revised.size());
    CHECK(same_event(output[0], revised[0]));
    CHECK(same_event(output[1], revised[1]));

    std::array future{event(0, 30)};
    REQUIRE(source.stage(span(kGrid, 2 * kGrid), 1, future).code ==
            playback::GeneratedEventStageCode::Staged);
    future[0] = event(0, 31);
    REQUIRE(source.stage(span(kGrid, 2 * kGrid), 2, future).code ==
            playback::GeneratedEventStageCode::Revised);
    // The source owns its copy; later caller mutation cannot alter the commit.
    const auto expected_future = future[0];
    future[0] = event(0, 88);
    REQUIRE(source.commit_through(1, {{2 * kGrid}}, 2).code ==
            playback::GeneratedEventCommitCode::Committed);
    const auto consumed_future = source.pull(span(kGrid, 2 * kGrid), output);
    REQUIRE(consumed_future.code == playback::GeneratedEventPullCode::Ready);
    REQUIRE(consumed_future.event_count == 1);
    CHECK(consumed_future.commit_generation == 2);
    CHECK(same_event(output[0], expected_future));
}

TEST_CASE("A full committed ring rejects the whole quantized commit",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source, config(1, 3, 2));
    const std::array first{event(0, 1)};
    const std::array second{event(0, 2)};
    REQUIRE(source.stage(span(0, kGrid), 1, first));
    REQUIRE(source.stage(span(kGrid, 2 * kGrid), 1, second));
    REQUIRE(source.commit_through(1, {{kGrid}}, 1).committed_batches == 1);

    const auto rejected = source.commit_through(1, {{2 * kGrid}}, 2);
    CHECK(rejected.code == playback::GeneratedEventCommitCode::CommitRingFull);
    CHECK(rejected.committed_batches == 0);
    CHECK(source.stats().committed_batches == 1);

    std::array<playback::GeneratedEvent, 2> output{};
    REQUIRE(source.pull(span(0, kGrid), output).code == playback::GeneratedEventPullCode::Ready);
    const auto retried = source.commit_through(1, {{2 * kGrid}}, 2);
    REQUIRE(retried.code == playback::GeneratedEventCommitCode::Committed);
    REQUIRE(retried.committed_batches == 1);
    const auto consumed = source.pull(span(kGrid, 2 * kGrid), output);
    REQUIRE(consumed.code == playback::GeneratedEventPullCode::Ready);
    CHECK(same_event(output[0], second[0]));
}

TEST_CASE("Consumer starvation is event silence, exact lag, and a permanent elapsed span",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    std::array<playback::GeneratedEvent, 2> output{event(0, 77), event(0, 78)};

    const auto missed = source.pull(span(0, kGrid), output);
    REQUIRE(missed.code == playback::GeneratedEventPullCode::Starved);
    CHECK(missed.event_count == 0);
    CHECK(missed.lagged_ticks == static_cast<std::uint64_t>(kGrid));
    CHECK(missed.degradation == playback::GeneratedEventDegradation::Silence);
    CHECK(missed.flush_active_notes);
    CHECK(source.stats().starvation_events == 1);
    CHECK(source.stats().lagged_ticks == static_cast<std::uint64_t>(kGrid));

    const std::array late{event(0, 3)};
    CHECK(source.stage(span(0, kGrid), 1, late).code ==
          playback::GeneratedEventStageCode::FrozenSpan);

    const std::array on_time{event(0, 4)};
    REQUIRE(source.stage(span(kGrid, 2 * kGrid), 1, on_time));
    REQUIRE(source.commit_through(1, {{2 * kGrid}}, 1));
    const auto ready = source.pull(span(kGrid, 2 * kGrid), output);
    REQUIRE(ready.code == playback::GeneratedEventPullCode::Ready);
    CHECK(same_event(output[0], on_time[0]));
}

TEST_CASE("A committed future batch survives an earlier missing span",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    const std::array future{event(0, 9)};
    REQUIRE(source.stage(span(kGrid, 2 * kGrid), 1, future));
    REQUIRE(source.commit_through(1, {{2 * kGrid}}, 1));

    std::array<playback::GeneratedEvent, 2> output{};
    const auto missed = source.pull(span(0, kGrid), output);
    REQUIRE(missed.code == playback::GeneratedEventPullCode::Starved);
    CHECK(source.stats().late_batches == 0);

    const auto ready = source.pull(span(kGrid, 2 * kGrid), output);
    REQUIRE(ready.code == playback::GeneratedEventPullCode::Ready);
    CHECK(same_event(output[0], future[0]));
}

TEST_CASE("Discarding a stale batch flushes notes before serving a later ready batch",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    const std::array first{event(0, 1)};
    const std::array skipped{event(0, 2)};
    const std::array later{event(0, 3)};
    REQUIRE(source.stage(span(0, kGrid), 1, first));
    REQUIRE(source.stage(span(kGrid, 2 * kGrid), 1, skipped));
    REQUIRE(source.stage(span(2 * kGrid, 3 * kGrid), 1, later));
    REQUIRE(source.commit_through(1, {{3 * kGrid}}, 1));

    std::array<playback::GeneratedEvent, 1> output{};
    const auto initial = source.pull(span(0, kGrid), output);
    REQUIRE(initial.code == playback::GeneratedEventPullCode::Ready);
    CHECK_FALSE(initial.flush_active_notes);

    const auto after_gap = source.pull(span(2 * kGrid, 3 * kGrid), output);
    REQUIRE(after_gap.code == playback::GeneratedEventPullCode::Ready);
    CHECK(after_gap.flush_active_notes);
    CHECK(source.stats().late_batches == 1);
    CHECK(same_event(output[0], later[0]));
}

TEST_CASE("A staged batch cannot publish after its span permanently elapsed",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    const std::array late{event(0, 10)};
    REQUIRE(source.stage(span(0, kGrid), 1, late));

    std::array<playback::GeneratedEvent, 1> output{};
    REQUIRE(source.pull(span(0, kGrid), output).code == playback::GeneratedEventPullCode::Starved);
    const auto commit = source.commit_through(1, {{kGrid}}, 1);
    REQUIRE(commit.code == playback::GeneratedEventCommitCode::Committed);
    CHECK(commit.committed_batches == 0);
    CHECK(source.stats().committed_batches_ready == 0);
}

TEST_CASE("An out-of-order pull cannot regress the permanent elapsed frontier",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    std::array<playback::GeneratedEvent, 1> output{};

    REQUIRE(source.pull(span(2 * kGrid, 3 * kGrid), output).code ==
            playback::GeneratedEventPullCode::Starved);
    CHECK(source.pull(span(0, kGrid), output).code ==
          playback::GeneratedEventPullCode::InvalidRequest);

    const std::array reopened{event(0, 14)};
    CHECK(source.stage(span(kGrid, 2 * kGrid), 1, reopened).code ==
          playback::GeneratedEventStageCode::FrozenSpan);
}

TEST_CASE("Output overflow permanently consumes a batch without violating RT safety",
          "[playback][generated-events][rt-safety]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source, config(2, 2, 2));
    const std::array too_many{event(0, 11), event(kGrid / 2, 12)};
    REQUIRE(source.stage(span(0, kGrid), 1, too_many));
    REQUIRE(source.commit_through(1, {{kGrid}}, 7));

    std::array<playback::GeneratedEvent, 1> output{};
    playback::GeneratedEventPullResult overflow;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        overflow = source.pull(span(0, kGrid), output);
        allocations = probe.allocation_count();
    }
    REQUIRE(overflow.code == playback::GeneratedEventPullCode::OutputOverflow);
    CHECK(overflow.event_count == 0);
    CHECK(overflow.commit_generation == 7);
    CHECK(overflow.flush_active_notes);
    CHECK(allocations == 0);
    CHECK(source.stats().output_overflows == 1);
    CHECK(source.stage(span(0, kGrid), 2, too_many).code ==
          playback::GeneratedEventStageCode::FrozenSpan);

    const std::array next{event(0, 13)};
    REQUIRE(source.stage(span(kGrid, 2 * kGrid), 1, next));
    REQUIRE(source.commit_through(1, {{2 * kGrid}}, 8));
    REQUIRE(source.pull(span(kGrid, 2 * kGrid), output).code ==
            playback::GeneratedEventPullCode::Ready);
}

TEST_CASE("Deadline misses engage the producer-declared ladder in exact order",
          "[playback][generated-events]") {
    auto custom = config();
    custom.degradation.steps = {
        playback::GeneratedEventDegradation::UseSkeleton,
        playback::GeneratedEventDegradation::RepeatLastCommitted,
        playback::GeneratedEventDegradation::Silence,
    };
    playback::GeneratedEventSource source;
    prepare_epoch(source, custom);

    CHECK(source.record_deadline_miss() == playback::GeneratedEventDegradation::UseSkeleton);
    CHECK(source.record_deadline_miss() ==
          playback::GeneratedEventDegradation::RepeatLastCommitted);
    CHECK(source.record_deadline_miss() == playback::GeneratedEventDegradation::Silence);
    CHECK(source.record_deadline_miss() == playback::GeneratedEventDegradation::Silence);
    auto stats = source.stats();
    CHECK(stats.deadline_misses == 4);
    CHECK(stats.degradation_uses == std::array<std::uint64_t, 3>{1, 1, 2});

    source.record_deadline_met();
    CHECK(source.record_deadline_miss() == playback::GeneratedEventDegradation::UseSkeleton);
    CHECK(source.stats().deadline_misses == 5);
}

TEST_CASE("Starvation reports the producer-selected fallback policy",
          "[playback][generated-events]") {
    auto custom = config();
    custom.degradation.steps = {
        playback::GeneratedEventDegradation::UseSkeleton,
        playback::GeneratedEventDegradation::RepeatLastCommitted,
        playback::GeneratedEventDegradation::Silence,
    };
    playback::GeneratedEventSource source;
    prepare_epoch(source, custom);
    REQUIRE(source.record_deadline_miss() == playback::GeneratedEventDegradation::UseSkeleton);

    std::array<playback::GeneratedEvent, 1> output{};
    const auto starved = source.pull(span(0, kGrid), output);
    REQUIRE(starved.code == playback::GeneratedEventPullCode::Starved);
    CHECK(starved.event_count == 0);
    CHECK(starved.degradation == playback::GeneratedEventDegradation::UseSkeleton);
    CHECK(starved.flush_active_notes);
}

TEST_CASE("Generated event batches validate grid, event bounds, order, and epoch identity",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    const std::array good{event(0, 1), event(kGrid / 2, 2)};

    CHECK(source.stage(span(1, kGrid + 1), 1, good).code ==
          playback::GeneratedEventStageCode::NotQuantized);
    auto outside = good;
    outside[1].offset = {kGrid};
    CHECK(source.stage(span(0, kGrid), 1, outside).code ==
          playback::GeneratedEventStageCode::InvalidEvent);
    auto unsorted = good;
    unsorted[0].offset = {kGrid / 2};
    unsorted[1].offset = {0};
    CHECK(source.stage(span(0, kGrid), 1, unsorted).code ==
          playback::GeneratedEventStageCode::InvalidEvent);
    auto malformed = good;
    malformed[0].packet.word_count = 0;
    CHECK(source.stage(span(0, kGrid), 1, malformed).code ==
          playback::GeneratedEventStageCode::InvalidEvent);
    malformed = good;
    malformed[0].packet.words[0] = 0x40903c00u;
    malformed[0].packet.word_count = 1;
    CHECK(source.stage(span(0, kGrid), 1, malformed).code ==
          playback::GeneratedEventStageCode::InvalidEvent);

    REQUIRE(source.stage(span(0, kGrid, 1), 1, good));
    REQUIRE(source.commit_through(1, {{kGrid}}, 1));
    std::array<playback::GeneratedEvent, 2> output{};

    const auto wrong_epoch = source.pull(span(0, kGrid, 2), output);
    CHECK(wrong_epoch.code == playback::GeneratedEventPullCode::InvalidRequest);
    CHECK(source.stats().late_batches == 0);
    CHECK(source.pull(span(0, kGrid, 1), output).code == playback::GeneratedEventPullCode::Ready);
}

TEST_CASE("Epoch transition invalidates pending work and preserves generation monotonicity",
          "[playback][generated-events]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    const std::array old{event(0, 1)};
    REQUIRE(source.stage(span(0, kGrid, 1), 1, old));
    REQUIRE(source.commit_through(1, {{kGrid}}, 4));
    CHECK(source.record_deadline_miss() ==
          playback::GeneratedEventDegradation::RepeatLastCommitted);
    const auto before_transition = source.stats();

    REQUIRE(source.begin_playback_epoch(2));
    CHECK(source.current_degradation() == playback::GeneratedEventDegradation::None);
    const auto after_transition = source.stats();
    CHECK(after_transition.deadline_misses == before_transition.deadline_misses);
    CHECK(after_transition.degradation_uses == before_transition.degradation_uses);
    CHECK_FALSE(source.begin_playback_epoch(2));
    CHECK_FALSE(source.begin_playback_epoch(1));
    CHECK(source.stats().committed_batches_ready == 0);
    CHECK(source.stage(span(0, kGrid, 1), 2, old).code ==
          playback::GeneratedEventStageCode::WrongEpoch);
    CHECK(source.commit_through(1, {{kGrid}}, 5).code ==
          playback::GeneratedEventCommitCode::WrongEpoch);
    CHECK(source.commit_through(2, {{kGrid}}, 4).code ==
          playback::GeneratedEventCommitCode::StaleGeneration);

    const auto empty_commit = source.commit_through(2, {{kGrid}}, 5);
    REQUIRE(empty_commit.code == playback::GeneratedEventCommitCode::Committed);
    CHECK(empty_commit.committed_batches == 0);
    CHECK(source.stage(span(0, kGrid, 2), 1, old).code ==
          playback::GeneratedEventStageCode::FrozenSpan);
}

TEST_CASE("Generated event ready and starvation pulls allocate nothing and take no lock",
          "[playback][generated-events][rt-safety]") {
    playback::GeneratedEventSource source;
    prepare_epoch(source);
    const std::array generated{event(0, 1)};
    REQUIRE(source.stage(span(0, kGrid), 1, generated));
    REQUIRE(source.commit_through(1, {{kGrid}}, 1));
    std::array<playback::GeneratedEvent, 2> output{};
    CHECK(source.record_deadline_miss() ==
          playback::GeneratedEventDegradation::RepeatLastCommitted);

    bool control_saw_allocation = false;
    {
        test::RtAllocationProbe control;
        auto* escaping = new float[g_generated_event_escaping_size];
        escaping[0] = 1.0f;
        g_generated_event_escaping_block = escaping;
        control_saw_allocation = control.saw_allocation();
        delete[] g_generated_event_escaping_block;
    }
    REQUIRE(control_saw_allocation);

    std::size_t ready_allocations = 1;
    playback::GeneratedEventPullResult ready;
    {
        test::ScopedRtProcessProbe probe;
        ready = source.pull(span(0, kGrid), output);
        ready_allocations = probe.allocation_count();
    }
    REQUIRE(ready.code == playback::GeneratedEventPullCode::Ready);
    CHECK(ready_allocations == 0);
    CHECK(source.current_degradation() == playback::GeneratedEventDegradation::RepeatLastCommitted);

    std::size_t starved_allocations = 1;
    playback::GeneratedEventPullResult starved;
    {
        test::ScopedRtProcessProbe probe;
        starved = source.pull(span(kGrid, 2 * kGrid), output);
        starved_allocations = probe.allocation_count();
    }
    REQUIRE(starved.code == playback::GeneratedEventPullCode::Starved);
    CHECK(starved.flush_active_notes);
    CHECK(starved_allocations == 0);
}
