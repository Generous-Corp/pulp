#include <catch2/catch_test_macros.hpp>
#include <pulp/osc/osc.hpp>
#include <pulp/events/interprocess_connection.hpp>
#include "../examples/cv-control/cv_control.hpp"
#include <variant>

TEST_CASE("OSC malformed packet is rejected", "[external-input][osc]") {
    const std::uint8_t malformed[] = {0, 1, 2, 3, 4};
    const auto message = pulp::osc::decode(malformed, sizeof(malformed));
    REQUIRE(message.address.empty());
}

TEST_CASE("OSC wrong-type arguments are rejected before frame publication", "[external-input][osc]") {
    pulp::osc::Message message("/pulp/control");
    message.add(std::string("not-a-float")).add(0.8f).add(std::int32_t{1});
    REQUIRE(message.args.size() == 3);
    REQUIRE_FALSE(std::holds_alternative<float>(message.args[0]));
}

TEST_CASE("IPC connection loss publishes unavailable through the recipe callback", "[external-input][ipc]") {
    struct Probe final : pulp::events::InterprocessConnection {
        bool lost = false;
        void connection_lost() override { lost = true; }
    } probe;
    probe.connection_lost();
    REQUIRE(probe.lost);
}

TEST_CASE("reserved MIDI packet rejects invalid status and data bytes", "[external-input][midi]") {
    using pulp::examples::cv_control::decode_midi_control;
    REQUIRE_FALSE(decode_midi_control(0xee, 0, 0, 1).has_value());
    REQUIRE_FALSE(decode_midi_control(0xef, 128, 0, 1).has_value());
    REQUIRE_FALSE(decode_midi_control(0xef, 0, 128, 1).has_value());
}
