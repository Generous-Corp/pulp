#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/interaction_c.h>
#include <pulp/runtime/interaction_event.hpp>

#include <cstddef>
#include <cstring>
#include <vector>

using pulp::runtime::InteractionEvent;
using pulp::runtime::InteractionPhase;

namespace {

PulpInteractionEvent make(uint32_t epoch, uint64_t sequence, uint32_t object, uint8_t phase,
                          float x = 0.25f, float y = 0.75f) {
    PulpInteractionEvent event{};
    event.source_id = 7;
    event.epoch = epoch;
    event.sequence = sequence;
    event.provider_time_ns = 1000 + sequence;
    event.object_id = object;
    event.phase = phase;
    event.x = x;
    event.y = y;
    event.strength = 0.5f;
    event.confidence = 1.0f;
    return event;
}

std::vector<PulpInteractionEvent> drain_ui(PulpInteractionRouter* router) {
    std::vector<PulpInteractionEvent> out;
    PulpInteractionEvent event{};
    while (pulp_interaction_pop_ui(router, &event))
        out.push_back(event);
    return out;
}

} // namespace

TEST_CASE("the C interaction event has the C++ event's layout, field by field",
          "[runtime][interaction-event][interaction-c]") {
    // Literal wire offsets: a Swift or other C consumer mirrors these numbers.
    CHECK(sizeof(PulpInteractionEvent) == 48);
    CHECK(offsetof(PulpInteractionEvent, source_id) == 0);
    CHECK(offsetof(PulpInteractionEvent, epoch) == 4);
    CHECK(offsetof(PulpInteractionEvent, sequence) == 8);
    CHECK(offsetof(PulpInteractionEvent, provider_time_ns) == 16);
    CHECK(offsetof(PulpInteractionEvent, object_id) == 24);
    CHECK(offsetof(PulpInteractionEvent, phase) == 28);
    CHECK(offsetof(PulpInteractionEvent, reserved) == 29);
    CHECK(offsetof(PulpInteractionEvent, x) == 32);
    CHECK(offsetof(PulpInteractionEvent, y) == 36);
    CHECK(offsetof(PulpInteractionEvent, strength) == 40);
    CHECK(offsetof(PulpInteractionEvent, confidence) == 44);

    CHECK(offsetof(InteractionEvent, source_id) == offsetof(PulpInteractionEvent, source_id));
    CHECK(offsetof(InteractionEvent, epoch) == offsetof(PulpInteractionEvent, epoch));
    CHECK(offsetof(InteractionEvent, sequence) == offsetof(PulpInteractionEvent, sequence));
    CHECK(offsetof(InteractionEvent, provider_time_ns) ==
          offsetof(PulpInteractionEvent, provider_time_ns));
    CHECK(offsetof(InteractionEvent, object_id) == offsetof(PulpInteractionEvent, object_id));
    CHECK(offsetof(InteractionEvent, phase) == offsetof(PulpInteractionEvent, phase));
    CHECK(offsetof(InteractionEvent, reserved) == offsetof(PulpInteractionEvent, reserved));
    CHECK(offsetof(InteractionEvent, x) == offsetof(PulpInteractionEvent, x));
    CHECK(offsetof(InteractionEvent, y) == offsetof(PulpInteractionEvent, y));
    CHECK(offsetof(InteractionEvent, strength) == offsetof(PulpInteractionEvent, strength));
    CHECK(offsetof(InteractionEvent, confidence) == offsetof(PulpInteractionEvent, confidence));
}

TEST_CASE("a C event survives the router with every field intact",
          "[runtime][interaction-event][interaction-c]") {
    auto* router = pulp_interaction_router_create(0);
    REQUIRE(router != nullptr);
    auto sent = make(3, 11, 42, PULP_INTERACTION_PHASE_PRESS, 0.125f, 0.875f);
    sent.strength = 0.625f;
    sent.confidence = 0.75f;
    REQUIRE(pulp_interaction_publish(router, &sent));

    PulpInteractionEvent audio{};
    REQUIRE(pulp_interaction_pop_audio(router, &audio));
    const auto ui = drain_ui(router);
    REQUIRE(ui.size() == 1);
    CHECK(std::memcmp(&ui[0], &sent, sizeof(sent)) == 0);
    CHECK(std::memcmp(&audio, &sent, sizeof(sent)) == 0);
    pulp_interaction_router_destroy(router);
}

TEST_CASE("the router, not the caller, cancels held interactions on an epoch change",
          "[runtime][interaction-event][interaction-c]") {
    auto* router = pulp_interaction_router_create(0);
    REQUIRE(router != nullptr);
    const auto press = make(1, 1, 5, PULP_INTERACTION_PHASE_PRESS, 0.2f, 0.3f);
    const auto move = make(1, 2, 5, PULP_INTERACTION_PHASE_MOVE, 0.4f, 0.5f);
    REQUIRE(pulp_interaction_publish(router, &press));
    REQUIRE(pulp_interaction_publish(router, &move));
    // A new epoch starts with a press on another object; the tracker cancels
    // the held one first, carrying that object's last position.
    const auto next = make(2, 1, 9, PULP_INTERACTION_PHASE_PRESS, 0.9f, 0.1f);
    REQUIRE(pulp_interaction_publish(router, &next));

    const auto ui = drain_ui(router);
    REQUIRE(ui.size() == 4);
    CHECK(ui[0].phase == PULP_INTERACTION_PHASE_PRESS);
    CHECK(ui[1].phase == PULP_INTERACTION_PHASE_MOVE);
    CHECK(ui[2].phase == PULP_INTERACTION_PHASE_CANCEL);
    CHECK(ui[2].object_id == 5);
    CHECK(ui[2].epoch == 2);
    CHECK(ui[2].x == Catch::Approx(0.4f));
    CHECK(ui[2].y == Catch::Approx(0.5f));
    CHECK(ui[3].phase == PULP_INTERACTION_PHASE_PRESS);
    CHECK(ui[3].object_id == 9);

    // A stale epoch is refused without producing anything.
    const auto stale = make(1, 3, 5, PULP_INTERACTION_PHASE_MOVE);
    CHECK_FALSE(pulp_interaction_publish(router, &stale));
    CHECK(drain_ui(router).empty());
    pulp_interaction_router_destroy(router);
}

TEST_CASE("the C entry points refuse null arguments",
          "[runtime][interaction-event][interaction-c]") {
    auto* router = pulp_interaction_router_create(0);
    const auto event = make(1, 1, 1, PULP_INTERACTION_PHASE_PRESS);
    PulpInteractionEvent out{};
    CHECK_FALSE(pulp_interaction_publish(nullptr, &event));
    CHECK_FALSE(pulp_interaction_publish(router, nullptr));
    CHECK_FALSE(pulp_interaction_pop_ui(nullptr, &out));
    CHECK_FALSE(pulp_interaction_pop_ui(router, nullptr));
    CHECK_FALSE(pulp_interaction_check_timeout(nullptr, 1));
    pulp_interaction_reset(nullptr);
    pulp_interaction_router_destroy(nullptr);
    pulp_interaction_router_destroy(router);
}
