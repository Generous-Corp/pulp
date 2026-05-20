#include <catch2/catch_test_macros.hpp>

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
