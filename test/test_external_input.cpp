#include "harness/rt_allocation_probe.hpp"
#include "support/external_input_replay.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <atomic>
#include <limits>
#include <set>
#include <thread>
#include <pulp/runtime/external_input.hpp>
#include <pulp/runtime/interaction_event.hpp>
#include <pulp/runtime/interaction_router.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <type_traits>

using namespace pulp::runtime;
static_assert(std::is_trivially_copyable_v<ExternalInputFrame<1>>);
static_assert(std::is_trivially_copyable_v<ExternalInputFrame<64>>);
// Wire values of InteractionPhase are persisted by replay and provider payloads: appending members only.
static_assert(static_cast<std::uint8_t>(InteractionPhase::press) == 0);
static_assert(static_cast<std::uint8_t>(InteractionPhase::hold) == 1);
static_assert(static_cast<std::uint8_t>(InteractionPhase::move) == 2);
static_assert(static_cast<std::uint8_t>(InteractionPhase::release) == 3);
static_assert(static_cast<std::uint8_t>(InteractionPhase::cancel) == 4);

TEST_CASE("interaction batches preserve identity and bounded capacity",
          "[runtime][interaction-event]") {
    InteractionBatch<2> batch;
    REQUIRE(batch.append({.source_id = 4,
                          .epoch = 2,
                          .sequence = 10,
                          .provider_time_ns = 100,
                          .object_id = 7,
                          .phase = InteractionPhase::press,
                          .x = .25f,
                          .y = .5f,
                          .confidence = .9f}));
    REQUIRE(batch.append({.source_id = 4,
                          .epoch = 2,
                          .sequence = 11,
                          .provider_time_ns = 110,
                          .object_id = 7,
                          .phase = InteractionPhase::release,
                          .x = .3f,
                          .y = .5f,
                          .confidence = .9f}));
    CHECK(batch.count == 2);
    REQUIRE(batch.events_view().size() == 2);
    CHECK(batch.events[0].object_id == batch.events[1].object_id);
    CHECK_FALSE(batch.append({.source_id = 4,
                              .epoch = 2,
                              .sequence = 12,
                              .provider_time_ns = 120,
                              .object_id = 9,
                              .phase = InteractionPhase::press}));

    // Malformed state (count already past capacity) must be rejected, not written past the array.
    InteractionBatch<2> malformed;
    malformed.count = static_cast<std::uint8_t>(malformed.events.size() + 1);
    CHECK_FALSE(malformed.append({}));
}

TEST_CASE("interaction event queue preserves ordered terminal events",
          "[runtime][interaction-event]") {
    InteractionEventQueue<2> queue;
    const auto press = InteractionEvent{.source_id = 9,
                                        .epoch = 1,
                                        .sequence = 1,
                                        .object_id = 3,
                                        .phase = InteractionPhase::press,
                                        .x = .2f,
                                        .y = .4f,
                                        .confidence = .95f};
    auto release = press;
    release.sequence = 2;
    release.phase = InteractionPhase::release;
    REQUIRE(queue.try_push(press));
    REQUIRE(queue.try_push(release));
    CHECK_FALSE(queue.try_push(press));
    CHECK(queue.overflow_count() == 1);

    InteractionEvent observed;
    REQUIRE(queue.try_pop(observed));
    CHECK(observed.phase == InteractionPhase::press);
    REQUIRE(queue.try_pop(observed));
    CHECK(observed.phase == InteractionPhase::release);
    CHECK(queue.empty());
}

TEST_CASE("interaction tracker cancels held objects on restart and timeout",
          "[runtime][interaction-event]") {
    InteractionTracker<2> tracker;
    InteractionBatch<3> result;
    const auto press = InteractionEvent{.source_id = 4,
                                        .epoch = 1,
                                        .sequence = 1,
                                        .provider_time_ns = 100,
                                        .object_id = 7,
                                        .phase = InteractionPhase::press,
                                        .x = .25f,
                                        .y = .5f,
                                        .confidence = .9f};
    REQUIRE(tracker.submit(press, result) == InteractionVerdict::accepted);
    REQUIRE(tracker.active_count() == 1);

    auto restarted = press;
    restarted.epoch = 2;
    restarted.provider_time_ns = 200;
    restarted.object_id = 8;
    REQUIRE(tracker.submit(restarted, result) == InteractionVerdict::accepted);
    REQUIRE(result.events_view().size() == 2);
    CHECK(result.events_view()[0].phase == InteractionPhase::cancel);
    CHECK(result.events_view()[0].object_id == 7);
    CHECK(result.events_view()[1].phase == InteractionPhase::press);
    CHECK(tracker.active_count() == 1);

    const auto timeout = tracker.advance(128, 128);
    REQUIRE(timeout.events_view().size() == 1);
    CHECK(timeout.events_view()[0].phase == InteractionPhase::cancel);
    CHECK(timeout.events_view()[0].object_id == 8);
    CHECK(tracker.active_count() == 0);
}

TEST_CASE("full interaction tracker delivers the press that starts a new epoch",
          "[runtime][interaction-event][interaction-router]") {
    constexpr std::size_t kObjects = 64;
    auto press = InteractionEvent{.source_id = 4,
                                  .epoch = 1,
                                  .sequence = 0,
                                  .phase = InteractionPhase::press,
                                  .x = .5f,
                                  .y = .5f,
                                  .confidence = 1.0f};

    InteractionTracker<kObjects> tracker;
    InteractionTracker<kObjects>::Result result;
    for (std::uint32_t id = 0; id < kObjects; ++id) {
        press.sequence = id + 1;
        press.object_id = id;
        REQUIRE(tracker.submit(press, result) == InteractionVerdict::accepted);
    }
    REQUIRE(tracker.active_count() == kObjects);

    auto restart = press;
    restart.epoch = 2;
    restart.sequence = 1;
    restart.object_id = 1000;
    REQUIRE(tracker.submit(restart, result) == InteractionVerdict::accepted);
    const auto events = result.events_view();
    REQUIRE(events.size() == kObjects + 1);
    for (std::size_t i = 0; i < kObjects; ++i)
        CHECK(events[i].phase == InteractionPhase::cancel);
    CHECK(events.back().phase == InteractionPhase::press);
    CHECK(events.back().object_id == 1000);
    CHECK(tracker.active_count() == 1);

    // The default router size has the same bound; the press must reach both consumers.
    InteractionRouter<256> router;
    for (std::uint32_t id = 0; id < kObjects; ++id) {
        press.sequence = id + 1;
        press.object_id = id;
        REQUIRE(router.publish(press));
    }
    REQUIRE(router.publish(restart));
    InteractionEvent observed{};
    for (int consumer = 0; consumer < 2; ++consumer) {
        std::size_t presses = 0, cancels = 0;
        bool saw_restart = false;
        while (consumer == 0 ? router.try_pop_ui(observed) : router.try_pop_audio(observed)) {
            if (observed.phase == InteractionPhase::press) {
                ++presses;
                saw_restart = saw_restart || observed.object_id == 1000;
            } else if (observed.phase == InteractionPhase::cancel) {
                ++cancels;
            }
        }
        CHECK(presses == kObjects + 1);
        CHECK(cancels == kObjects);
        CHECK(saw_restart);
    }
}

TEST_CASE("interaction tracker cancels every object on a restart terminal event",
          "[runtime][interaction-event]") {
    InteractionTracker<2> tracker;
    InteractionBatch<3> result;
    const auto press = [](std::uint32_t object_id, std::uint64_t sequence) {
        return InteractionEvent{.source_id = 8,
                                .epoch = 1,
                                .sequence = sequence,
                                .object_id = object_id,
                                .phase = InteractionPhase::press,
                                .x = .2f,
                                .y = .4f,
                                .confidence = .9f};
    };
    REQUIRE(tracker.submit(press(1, 1), result) == InteractionVerdict::accepted);
    REQUIRE(tracker.submit(press(2, 2), result) == InteractionVerdict::accepted);
    auto restart_release = press(1, 3);
    restart_release.epoch = 2;
    restart_release.phase = InteractionPhase::release;
    REQUIRE(tracker.submit(restart_release, result) == InteractionVerdict::accepted);
    REQUIRE(result.events_view().size() == 2);
    CHECK(result.events_view()[0].phase == InteractionPhase::cancel);
    CHECK(result.events_view()[0].object_id == 1);
    CHECK(result.events_view()[1].phase == InteractionPhase::cancel);
    CHECK(result.events_view()[1].object_id == 2);
    CHECK(tracker.active_count() == 0);
}

TEST_CASE("interaction tracker expires each object independently and rejects stale epochs",
          "[runtime][interaction-event]") {
    InteractionTracker<2> tracker;
    InteractionBatch<3> result;
    const auto press_a = InteractionEvent{.source_id = 4,
                                          .epoch = 2,
                                          .sequence = 1,
                                          .provider_time_ns = 100,
                                          .object_id = 1,
                                          .phase = InteractionPhase::press,
                                          .x = .2f,
                                          .y = .4f,
                                          .strength = .8f,
                                          .confidence = .9f};
    auto press_b = press_a;
    press_b.sequence = 2;
    press_b.provider_time_ns = 110;
    press_b.object_id = 2;
    REQUIRE(tracker.submit(press_a, result) == InteractionVerdict::accepted);
    REQUIRE(tracker.submit(press_b, result) == InteractionVerdict::accepted);

    auto hold_b = press_b;
    hold_b.sequence = 3;
    hold_b.provider_time_ns = 120;
    hold_b.phase = InteractionPhase::hold;
    tracker.submit(hold_b, result);
    tracker.advance(5, 10);
    hold_b.sequence = 4;
    hold_b.provider_time_ns = 130;
    tracker.submit(hold_b, result);
    const auto first_expiry = tracker.advance(5, 10);
    REQUIRE(first_expiry.events_view().size() == 1);
    CHECK(first_expiry.events_view()[0].object_id == 1);
    CHECK(tracker.active_count() == 1);

    auto stale = hold_b;
    stale.epoch = 1;
    stale.sequence = 99;
    CHECK(tracker.submit(stale, result) == InteractionVerdict::rejected_order);
    CHECK(tracker.active_count() == 1);
}

TEST_CASE("interaction tracker clears an invalid terminating event and resets source",
          "[runtime][interaction-event]") {
    InteractionTracker<1> tracker;
    InteractionBatch<2> result;
    const auto press = InteractionEvent{.source_id = 4,
                                        .epoch = 1,
                                        .sequence = 1,
                                        .object_id = 3,
                                        .phase = InteractionPhase::press,
                                        .x = .2f,
                                        .y = .4f,
                                        .strength = .8f,
                                        .confidence = .9f};
    REQUIRE(tracker.submit(press, result) == InteractionVerdict::accepted);
    auto release = press;
    release.sequence = 2;
    release.phase = InteractionPhase::release;
    release.x = 2.0f;
    release.y = std::numeric_limits<float>::quiet_NaN();
    release.strength = 2.0f;
    REQUIRE(tracker.submit(release, result) == InteractionVerdict::accepted);
    CHECK(tracker.active_count() == 0);
    CHECK(result.events_view()[0].phase == InteractionPhase::release);
    CHECK(result.events_view()[0].x == .2f);
    CHECK(tracker.reset().count == 0);
}

template <std::size_t N = 3>
static ExternalInputFrame<N> frame(std::uint32_t epoch = 1, std::uint64_t sequence = 1) {
    ExternalInputFrame<N> f;
    f.source_id = 7;
    f.epoch = epoch;
    f.sequence = sequence;
    f.provider_time_ns = sequence * 10;
    f.status = ExternalInputStatus::valid;
    f.confidence = 0.9f;
    f.count = static_cast<std::uint8_t>(N);
    for (std::size_t i = 0; i < N; ++i)
        f.values[i] = static_cast<float>(i + sequence);
    return f;
}

TEST_CASE("external input accepts and exposes a complete frame", "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.5f, 10, 3};
    REQUIRE(gate.submit(frame(), p) == ExternalInputVerdict::accepted);
    REQUIRE(gate.valid());
    REQUIRE(gate.values().size() == 3);
    REQUIRE(gate.value(1, -1) == 2);
    REQUIRE(gate.counters().accepted == 1);
    REQUIRE(gate.counters().epochs == 1);
}

TEST_CASE("external input verdict ordering and lifecycle", "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.5f, 5, 3};
    auto f = frame();
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::accepted);
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::unchanged);
    auto bad_source = f;
    bad_source.source_id = 8;
    bad_source.sequence = 2;
    REQUIRE(gate.submit(bad_source, p) == ExternalInputVerdict::rejected_source);
    auto denied = f;
    denied.sequence = 2;
    denied.status = ExternalInputStatus::denied;
    REQUIRE(gate.submit(denied, p) == ExternalInputVerdict::rejected_status);
    REQUIRE_FALSE(gate.valid());
    REQUIRE(gate.last_status() == ExternalInputStatus::denied);
    auto malformed = frame(1, 3);
    malformed.count = 0;
    REQUIRE(gate.submit(malformed, p) == ExternalInputVerdict::rejected_malformed);
    auto low = frame(1, 4);
    low.confidence = 0.1f;
    REQUIRE(gate.submit(low, p) == ExternalInputVerdict::rejected_confidence);
    auto accepted = frame(1, 5);
    REQUIRE(gate.submit(accepted, p) == ExternalInputVerdict::accepted);
    auto old = frame(1, 4);
    REQUIRE(gate.submit(old, p) == ExternalInputVerdict::rejected_order);
    REQUIRE_FALSE(gate.advance(4, p));
    REQUIRE(gate.valid());
    REQUIRE(gate.advance(1, p));
    REQUIRE_FALSE(gate.valid());
    REQUIRE(gate.counters().expired == 1);
    auto restart = frame(2, 1);
    REQUIRE(gate.submit(restart, p) == ExternalInputVerdict::accepted);
    REQUIRE(gate.epoch() == 2);
}

TEST_CASE("external input stale status frame cannot knock out a newer value",
          "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.5f, 0, 3};
    REQUIRE(gate.submit(frame(1, 5), p) == ExternalInputVerdict::accepted);
    auto paused = frame(1, 2);
    paused.status = ExternalInputStatus::paused;
    REQUIRE(gate.submit(paused, p) == ExternalInputVerdict::rejected_order);
    REQUIRE(gate.valid());
    REQUIRE(gate.last_status() == ExternalInputStatus::valid);
    REQUIRE(gate.sequence() == 5);
    REQUIRE(gate.counters().rejected_status == 0);
    REQUIRE(gate.counters().rejected_order == 1);
}

TEST_CASE("external input ordering watermark survives expiry", "[runtime][external-input]") {
    ExternalInputPolicy p{7, 0.5f, 5, 3};
    SECTION("status expiry") {
        ExternalInputGate<3> gate;
        REQUIRE(gate.submit(frame(1, 10), p) == ExternalInputVerdict::accepted);
        auto denied = frame(1, 11);
        denied.status = ExternalInputStatus::denied;
        REQUIRE(gate.submit(denied, p) == ExternalInputVerdict::rejected_status);
        REQUIRE_FALSE(gate.valid());
        REQUIRE(gate.submit(frame(1, 9), p) == ExternalInputVerdict::rejected_order);
        REQUIRE_FALSE(gate.valid());
        REQUIRE(gate.submit(frame(1, 12), p) == ExternalInputVerdict::accepted);
    }
    SECTION("age expiry") {
        ExternalInputGate<3> gate;
        REQUIRE(gate.submit(frame(2, 10), p) == ExternalInputVerdict::accepted);
        REQUIRE(gate.advance(5, p));
        REQUIRE_FALSE(gate.valid());
        REQUIRE(gate.submit(frame(2, 9), p) == ExternalInputVerdict::rejected_order);
        REQUIRE(gate.submit(frame(1, 50), p) == ExternalInputVerdict::rejected_order);
        REQUIRE_FALSE(gate.valid());
        REQUIRE(gate.submit(frame(2, 11), p) == ExternalInputVerdict::accepted);
    }
}

TEST_CASE("external input checks source before reporting unchanged", "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.5f, 0, 3};
    REQUIRE(gate.submit(frame(1, 5), p) == ExternalInputVerdict::accepted);
    auto wrong = frame(1, 5);
    wrong.source_id = 8;
    REQUIRE(gate.submit(wrong, p) == ExternalInputVerdict::rejected_source);
    REQUIRE(gate.counters().unchanged == 0);
    REQUIRE(gate.valid());
}

TEST_CASE("external input rejects non-finite and wrong shape", "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.0f, 0, 3};
    auto f = frame();
    f.values[1] = std::numeric_limits<float>::quiet_NaN();
    f.sequence = 2;
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::rejected_malformed);
    f = frame(1, 3);
    f.count = 2;
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::rejected_malformed);
    f = frame(1, 4);
    f.confidence = 2.0f;
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::rejected_confidence);
}

TEST_CASE("external input gate and triple buffer are allocation free",
          "[runtime][external-input]") {
    TripleBuffer<ExternalInputFrame<3>> buffer;
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0, 0, 3};
    pulp::test::RtAllocationProbe probe;
    for (std::uint64_t i = 1; i <= 10000; ++i) {
        auto f = frame(1, i);
        buffer.write(f);
        REQUIRE(gate.submit(buffer.read(), p) ==
                (i == 1 ? ExternalInputVerdict::accepted : ExternalInputVerdict::accepted));
    }
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("external input replay receipts classify positive, negative, and inconclusive fixtures",
          "[runtime][external-input]") {
    for (const auto* name :
         {"imu-pause-resume.jsonl", "negative-control.jsonl", "negative-values.jsonl",
          "negative-verdict.jsonl", "negative-extra-value.jsonl", "negative-capacity.jsonl",
          "negative-status.jsonl", "zero-expect.jsonl"}) {
        ExternalInputGate<64> gate;
        ExternalInputPolicy policy{1, 0.6f, 12000, 0};
        const auto receipt = pulp::test::run_external_input_replay(
            std::filesystem::path("test/fixtures/external-input") / name, gate, policy);
        if (std::string(name) == "imu-pause-resume.jsonl")
            REQUIRE(receipt.result == "pass");
        if (std::string(name) == "negative-control.jsonl")
            REQUIRE(receipt.result == "fail");
        if (std::string(name) == "negative-values.jsonl")
            REQUIRE(receipt.result == "fail");
        if (std::string(name) == "negative-verdict.jsonl")
            REQUIRE(receipt.result == "fail");
        if (std::string(name) == "negative-extra-value.jsonl" ||
            std::string(name) == "negative-capacity.jsonl" ||
            std::string(name) == "negative-status.jsonl")
            REQUIRE(receipt.result == "fail");
        if (std::string(name) == "zero-expect.jsonl")
            REQUIRE(receipt.result == "inconclusive");
    }
}

TEST_CASE("TouchDesigner sharing recipe fixture is replayed through the external input gate",
          "[runtime][external-input][touchdesigner]") {
    ExternalInputGate<64> gate;
    ExternalInputPolicy policy{7, 0.6f, 12000, 1};
    const auto receipt = pulp::test::run_external_input_replay(
        "test/fixtures/external-input/t7-touchdesigner-synthesized.jsonl", gate, policy);
    REQUIRE(receipt.failed == 0);
    REQUIRE(receipt.result == "pass");
    REQUIRE(receipt.frames == 4);
    REQUIRE(receipt.advances == 1);
    REQUIRE(receipt.expects == 5);
    REQUIRE(receipt.failed == 0);
}

TEST_CASE("interaction router orders events and cancels on epoch change",
          "[runtime][interaction-router]") {
    InteractionRouter<8, 8> router;
    REQUIRE(router.publish({.source_id = 1,
                            .epoch = 1,
                            .sequence = 1,
                            .object_id = 9,
                            .phase = InteractionPhase::press}));
    REQUIRE(router.publish({.source_id = 1,
                            .epoch = 1,
                            .sequence = 2,
                            .object_id = 9,
                            .phase = InteractionPhase::move}));
    REQUIRE(router.publish({.source_id = 1,
                            .epoch = 2,
                            .sequence = 1,
                            .object_id = 10,
                            .phase = InteractionPhase::press}));
    InteractionEvent event{};
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::press);
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::move);
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::cancel);
    CHECK(event.object_id == 9);
}

namespace {
std::uint64_t interaction_test_clock = 0;
std::uint64_t interaction_clock() noexcept {
    return interaction_test_clock;
}
} // namespace

TEST_CASE("interaction router cancels live objects after source timeout",
          "[runtime][interaction-router]") {
    interaction_test_clock = 100;
    InteractionRouter<8, 4> router(50, interaction_clock);
    REQUIRE(router.publish({.source_id = 2,
                            .epoch = 1,
                            .sequence = 1,
                            .object_id = 3,
                            .phase = InteractionPhase::press}));
    interaction_test_clock = 151;
    REQUIRE(router.check_timeout());
    InteractionEvent event{};
    REQUIRE(router.try_pop_audio(event));
    CHECK(event.phase == InteractionPhase::press);
    REQUIRE(router.try_pop_audio(event));
    CHECK(event.phase == InteractionPhase::cancel);
    CHECK(event.object_id == 3);
    CHECK_FALSE(router.check_timeout());
}

TEST_CASE("interaction router synthesizes cancellation after queue overflow",
          "[runtime][interaction-router]") {
    InteractionRouter<1, 4> router;
    REQUIRE(router.publish({.source_id = 3,
                            .epoch = 1,
                            .sequence = 1,
                            .object_id = 4,
                            .phase = InteractionPhase::press}));
    CHECK_FALSE(router.publish({.source_id = 3,
                                .epoch = 1,
                                .sequence = 2,
                                .object_id = 5,
                                .phase = InteractionPhase::press}));
    InteractionEvent event{};
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::press);
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::cancel);
    CHECK(event.object_id == 4);
}

TEST_CASE("interaction router keeps one pending cancel per overflowed consumer",
          "[runtime][interaction-router]") {
    InteractionRouter<1, 4> router;
    REQUIRE(router.publish({.source_id = 8,
                            .epoch = 1,
                            .sequence = 1,
                            .object_id = 12,
                            .phase = InteractionPhase::press}));
    CHECK_FALSE(router.publish({.source_id = 8,
                                .epoch = 1,
                                .sequence = 2,
                                .object_id = 12,
                                .phase = InteractionPhase::move}));
    InteractionEvent event{};
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::press);
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::cancel);
    CHECK(event.object_id == 12);
    CHECK_FALSE(router.try_pop_ui(event));
    REQUIRE(router.try_pop_audio(event));
    CHECK(event.phase == InteractionPhase::press);
    REQUIRE(router.try_pop_audio(event));
    CHECK(event.phase == InteractionPhase::cancel);
    CHECK(event.object_id == 12);
    CHECK_FALSE(router.try_pop_audio(event));
}

TEST_CASE("interaction router drains without heap allocation", "[runtime][interaction-router]") {
    REQUIRE(pulp::test::RtAllocationProbe::self_check());
    InteractionRouter<8, 4> router;
    REQUIRE(router.publish({.source_id = 4,
                            .epoch = 1,
                            .sequence = 1,
                            .object_id = 6,
                            .phase = InteractionPhase::press}));
    InteractionEvent event{};
    pulp::test::RtAllocationProbe probe;
    while (router.try_pop_ui(event)) {
    }
    CHECK_FALSE(probe.saw_allocation());
}

TEST_CASE("interaction router keeps a pending cancel for every live object on a full queue",
          "[runtime][interaction-router]") {
    InteractionRouter<1, 8> router;
    REQUIRE(router.publish({.source_id = 9,
                            .epoch = 1,
                            .sequence = 1,
                            .object_id = 21,
                            .phase = InteractionPhase::press}));
    CHECK_FALSE(router.publish({.source_id = 9,
                                .epoch = 1,
                                .sequence = 2,
                                .object_id = 22,
                                .phase = InteractionPhase::press}));
    CHECK_FALSE(router.publish({.source_id = 9,
                                .epoch = 1,
                                .sequence = 3,
                                .object_id = 23,
                                .phase = InteractionPhase::press}));
    CHECK(router.pending_cancel_dropped_count() == 0);
    for (const bool ui : {true, false}) {
        InteractionEvent event{};
        std::multiset<std::uint32_t> cancelled;
        int presses = 0;
        while (ui ? router.try_pop_ui(event) : router.try_pop_audio(event)) {
            if (event.phase == InteractionPhase::press)
                ++presses;
            else if (event.phase == InteractionPhase::cancel)
                cancelled.insert(event.object_id);
        }
        CHECK(presses == 1);
        // Every live object is cancelled exactly once at each consumer.
        CHECK(cancelled.count(21) == 1);
        CHECK(cancelled.count(22) == 1);
        CHECK(cancelled.count(23) == 1);
        CHECK(cancelled.size() == 3);
    }
}

TEST_CASE("interaction router rejects an invalid phase and ignores a zero clock",
          "[runtime][interaction-router]") {
    interaction_test_clock = 0;
    InteractionRouter<8, 4> router(50, interaction_clock);
    CHECK_FALSE(router.publish({.source_id = 10,
                                .epoch = 1,
                                .sequence = 1,
                                .object_id = 1,
                                .phase = InteractionPhase::invalid}));
    InteractionEvent event{};
    CHECK_FALSE(router.try_pop_ui(event));
    REQUIRE(router.publish({.source_id = 10,
                            .epoch = 1,
                            .sequence = 2,
                            .object_id = 1,
                            .phase = InteractionPhase::press}));
    // No usable clock reading: the timeout never fires and nothing is cancelled.
    CHECK_FALSE(router.check_timeout());
    CHECK_FALSE(router.check_timeout(1000));
    REQUIRE(router.try_pop_ui(event));
    CHECK(event.phase == InteractionPhase::press);
    CHECK_FALSE(router.try_pop_ui(event));
}

// Producer overflows tiny queues while two consumers drain concurrently. Under
// TSan this fails if the pending-cancel slots are plain (non-atomic) storage.
TEST_CASE("interaction router hands overflow cancels across threads",
          "[runtime][interaction-router][concurrency]") {
    InteractionRouter<2, 16> router;
    std::atomic<bool> done{false};
    std::atomic<int> bad_ui{0};
    std::atomic<int> bad_audio{0};
    auto consume = [&](bool ui, std::atomic<int>& bad) {
        InteractionEvent event{};
        auto pop = [&] { return ui ? router.try_pop_ui(event) : router.try_pop_audio(event); };
        for (;;) {
            const bool finished = done.load(std::memory_order_acquire);
            while (pop()) {
                if (event.phase == InteractionPhase::invalid || event.object_id < 1 ||
                    event.object_id > 16)
                    bad.fetch_add(1);
            }
            if (finished)
                break;
        }
    };
    std::thread ui_thread(consume, true, std::ref(bad_ui));
    std::thread audio_thread(consume, false, std::ref(bad_audio));
    std::uint64_t seq = 0;
    for (std::uint32_t epoch = 1; epoch <= 400; ++epoch) {
        seq = 0;
        for (std::uint32_t id = 1; id <= 16; ++id)
            (void)router.publish({.source_id = 11,
                                  .epoch = epoch,
                                  .sequence = ++seq,
                                  .object_id = id,
                                  .phase = InteractionPhase::press});
    }
    done.store(true, std::memory_order_release);
    ui_thread.join();
    audio_thread.join();
    CHECK(bad_ui.load() == 0);
    CHECK(bad_audio.load() == 0);
    CHECK(router.pending_cancel_dropped_count() == 0);
}

TEST_CASE("interaction tracker keeps active objects when a future-epoch event is malformed",
          "[runtime][interaction-event]") {
    InteractionTracker<2> tracker;
    InteractionBatch<3> result;
    const auto press = InteractionEvent{.source_id = 4,
                                        .epoch = 1,
                                        .sequence = 1,
                                        .provider_time_ns = 100,
                                        .object_id = 7,
                                        .phase = InteractionPhase::press,
                                        .x = .25f,
                                        .y = .5f,
                                        .strength = .75f,
                                        .confidence = .9f};
    REQUIRE(tracker.submit(press, result) == InteractionVerdict::accepted);

    auto bad = press;
    bad.epoch = 2;
    bad.object_id = 8;
    bad.x = 2.0f;
    CHECK(tracker.submit(bad, result) == InteractionVerdict::rejected_malformed);
    CHECK(result.events_view().empty());
    CHECK(tracker.active_count() == 1);

    // A valid restart still cancels, and the cancel keeps the contact strength.
    auto restarted = press;
    restarted.epoch = 2;
    restarted.object_id = 8;
    restarted.strength = 0.0f;
    REQUIRE(tracker.submit(restarted, result) == InteractionVerdict::accepted);
    REQUIRE(result.events_view().size() == 2);
    CHECK(result.events_view()[0].phase == InteractionPhase::cancel);
    CHECK(result.events_view()[0].strength == .75f);
}
