#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/parameter_event_queue.hpp>

#include <utility>
#include <vector>

using namespace pulp::host;

TEST_CASE("ParameterEventQueue sorts by sample offset without dropping payload",
          "[host][parameter-event-queue]") {
    ParameterEventQueue q;
    q.push(ParameterEvent{.param_id = 7, .sample_offset = 64, .value = 0.7f});
    q.push(ParameterEvent{.param_id = 3, .sample_offset = 0, .value = 0.3f});
    q.push(ParameterEvent{.param_id = 9, .sample_offset = 32, .value = 0.9f});

    REQUIRE_FALSE(q.empty());
    REQUIRE(q.size() == 3);

    q.sort();

    const auto& events = q.events();
    REQUIRE(events.size() == 3);
    REQUIRE(events[0].param_id == 3);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[0].value == 0.3f);
    REQUIRE(events[1].param_id == 9);
    REQUIRE(events[1].sample_offset == 32);
    REQUIRE(events[1].value == 0.9f);
    REQUIRE(events[2].param_id == 7);
    REQUIRE(events[2].sample_offset == 64);
    REQUIRE(events[2].value == 0.7f);
}

TEST_CASE("ParameterEventQueue supports move push, iteration, and reuse after clear",
          "[host][parameter-event-queue]") {
    ParameterEventQueue q;
    ParameterEvent moved{.param_id = 11, .sample_offset = -4, .value = -1.0f};
    q.push(std::move(moved));
    q.push(ParameterEvent{.param_id = 12, .sample_offset = 4, .value = 1.0f});

    std::vector<uint32_t> ids;
    for (const auto& event : q) {
        ids.push_back(event.param_id);
    }
    REQUIRE(ids == std::vector<uint32_t>{11, 12});

    q.clear();
    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);
    REQUIRE(q.begin() == q.end());

    q.push(ParameterEvent{.param_id = 42, .sample_offset = 1, .value = 0.5f});
    REQUIRE_FALSE(q.empty());
    REQUIRE(q.size() == 1);
    REQUIRE(q.events().front().param_id == 42);
}

TEST_CASE("ParameterEventQueue reports and resets overflow drops",
          "[host][parameter-event-queue][overflow]") {
    ParameterEventQueue q;
    for (std::size_t i = 0; i < q.capacity(); ++i) {
        REQUIRE(q.push(ParameterEvent{
            .param_id = static_cast<pulp::state::ParamID>(i),
            .sample_offset = static_cast<int32_t>(i),
            .value = 0.0f,
        }));
    }

    REQUIRE_FALSE(q.push(ParameterEvent{.param_id = 99, .sample_offset = 0, .value = 1.0f}));
    REQUIRE(q.overflowed());
    REQUIRE(q.dropped_event_count() == 1);
    REQUIRE(q.size() == q.capacity());

    q.clear();
    REQUIRE_FALSE(q.overflowed());
    REQUIRE(q.dropped_event_count() == 0);
    REQUIRE(q.empty());
}

TEST_CASE("ParameterEventQueue hot operations are allocation-free",
          "[host][parameter-event-queue][rt-safety]") {
    ParameterEventQueue q;

    std::size_t allocation_count = 0;
    std::size_t allocated_bytes = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (std::size_t i = 0; i < q.capacity(); ++i) {
            REQUIRE(q.push(ParameterEvent{
                .param_id = static_cast<pulp::state::ParamID>(i % 17),
                .sample_offset = static_cast<int32_t>(q.capacity() - i),
                .value = static_cast<float>(i),
            }));
        }
        REQUIRE_FALSE(q.push(ParameterEvent{.param_id = 99,
                                            .sample_offset = 0,
                                            .value = 1.0f}));
        q.sort();
        const auto events = q.events();
        REQUIRE(events.size() == q.capacity());
        q.clear();
        REQUIRE(q.empty());
        allocation_count = probe.allocation_count();
        allocated_bytes = probe.allocated_bytes();
    }

    REQUIRE(allocation_count == 0);
    REQUIRE(allocated_bytes == 0);
}

TEST_CASE("ParameterEventQueue sort() yields correct order under a full reversed flood",
          "[host][parameter-event-queue][rt-safety][flood]") {
    // Adversarial worst case for the insertion sort: a capacity-sized batch in
    // strictly descending sample_offset. Must still produce correct ascending
    // order, drop nothing, and never allocate (bounded O(n^2), n == kCapacity).
    ParameterEventQueue q;
    const auto cap = q.capacity();
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (std::size_t i = 0; i < cap; ++i) {
            // offset = cap - i  => first push has the largest offset (reversed).
            REQUIRE(q.push(ParameterEvent{
                .param_id = static_cast<pulp::state::ParamID>(i),
                .sample_offset = static_cast<int32_t>(cap - i),
                .value = static_cast<float>(i),
            }));
        }
        q.sort();
        allocation_count = probe.allocation_count();
    }

    REQUIRE(allocation_count == 0);
    const auto events = q.events();
    REQUIRE(events.size() == cap);
    for (std::size_t i = 1; i < events.size(); ++i) {
        REQUIRE(events[i - 1].sample_offset <= events[i].sample_offset);
    }
    // The smallest offset (1, from the last push) sorts to the front; the
    // largest (cap, from the first push) to the back.
    REQUIRE(events.front().sample_offset == 1);
    REQUIRE(events.back().sample_offset == static_cast<int32_t>(cap));
}

TEST_CASE("ParameterEventQueue sort() is stable across duplicate offsets at scale",
          "[host][parameter-event-queue][flood]") {
    // Many events share a handful of offsets; the stable sort must preserve the
    // push order within each offset bucket. param_id encodes push order.
    ParameterEventQueue q;
    const auto cap = q.capacity();
    for (std::size_t i = 0; i < cap; ++i) {
        REQUIRE(q.push(ParameterEvent{
            .param_id = static_cast<pulp::state::ParamID>(i),
            .sample_offset = static_cast<int32_t>(i % 4),  // 4 dense buckets
            .value = 0.0f,
        }));
    }

    q.sort();

    const auto events = q.events();
    REQUIRE(events.size() == cap);
    // Offsets non-decreasing overall; within each equal-offset run the
    // param_ids (push order) stay strictly increasing => stable.
    for (std::size_t i = 1; i < events.size(); ++i) {
        REQUIRE(events[i - 1].sample_offset <= events[i].sample_offset);
        if (events[i - 1].sample_offset == events[i].sample_offset) {
            REQUIRE(events[i - 1].param_id < events[i].param_id);
        }
    }
}

TEST_CASE("ParameterEventQueue exposes overflow telemetry",
          "[host][parameter-event-queue][telemetry]") {
    ParameterEventQueue q;
    REQUIRE(q.overflow_count() == 0);

    for (std::size_t i = 0; i < q.capacity(); ++i) {
        REQUIRE(q.push(ParameterEvent{
            .param_id = static_cast<uint32_t>(i + 1),
            .sample_offset = static_cast<int32_t>(i),
            .value = static_cast<float>(i),
        }));
    }

    REQUIRE_FALSE(q.push(ParameterEvent{.param_id = 9999, .sample_offset = 0, .value = 1.0f}));
    REQUIRE_FALSE(q.push(ParameterEvent{.param_id = 10000, .sample_offset = 1, .value = 2.0f}));
    REQUIRE(q.overflow_count() == 2);

    const auto full = q.telemetry();
    REQUIRE(full.size == q.capacity());
    REQUIRE(full.capacity == q.capacity());
    REQUIRE(full.overflow_count == 2);

    q.clear();
    REQUIRE(q.empty());
    REQUIRE(q.overflow_count() == 2);
    REQUIRE_FALSE(q.overflowed());
    REQUIRE(q.dropped_event_count() == 0);

    q.reset_overflow_count();
    REQUIRE(q.overflow_count() == 0);
    const auto reset = q.telemetry();
    REQUIRE(reset.size == 0);
    REQUIRE(reset.capacity == q.capacity());
    REQUIRE(reset.overflow_count == 0);
}

TEST_CASE("ParameterEventQueue telemetry path allocates zero times",
          "[host][parameter-event-queue][telemetry][rt-safety]") {
    ParameterEventQueue q;

    pulp::test::RtAllocationProbe probe;

    for (std::size_t i = 0; i < q.capacity(); ++i) {
        REQUIRE(q.push(ParameterEvent{
            .param_id = 1,
            .sample_offset = static_cast<int32_t>(i),
            .value = 0.5f,
        }));
    }

    REQUIRE_FALSE(q.push(ParameterEvent{.param_id = 1, .sample_offset = 0, .value = 1.0f}));
    (void)q.telemetry();
    q.reset_overflow_count();
    q.clear();

    REQUIRE_FALSE(probe.saw_allocation());
}
