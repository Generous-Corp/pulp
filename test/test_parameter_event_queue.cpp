#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/parameter_event_queue.hpp>

#include <utility>
#include <vector>

using namespace pulp::host;

TEST_CASE("ParameterEventQueue sorts by sample offset without dropping payload",
          "[host][parameter-event-queue][coverage][phase3]") {
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
          "[host][parameter-event-queue][coverage][phase3]") {
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

TEST_CASE("ParameterEventQueue exposes overflow telemetry",
          "[host][parameter-event-queue][telemetry][phase2]") {
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
          "[host][parameter-event-queue][telemetry][rt-safety][phase2]") {
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
