#include "../examples/cv-control/cv_control.hpp"
#include "../examples/cv-control/cv_control_processor.hpp"
#include <catch2/catch_test_macros.hpp>
#include <pulp/osc/osc.hpp>
#include <variant>

TEST_CASE("OSC malformed packet is rejected and valid control decodes", "[external-input][osc]") {
    const std::uint8_t malformed[] = {0, 1, 2, 3, 4};
    const auto rejected = pulp::osc::decode(malformed, sizeof(malformed));
    REQUIRE(rejected.address.empty());

    pulp::osc::Message source("/pulp/control");
    source.add(0.4f).add(0.8f).add(std::int32_t{1});
    const auto decoded =
        pulp::osc::decode(pulp::osc::encode(source).data(), pulp::osc::encode(source).size());
    const auto sample = pulp::examples::cv_control::decode_osc_control(decoded);
    REQUIRE(sample.has_value());
    REQUIRE(sample->valid);
}

TEST_CASE("OSC wrong-type arguments are rejected before frame publication",
          "[external-input][osc]") {
    pulp::examples::cv_control::FaceToneProcessor processor;
    pulp::osc::Message wrong("/pulp/control");
    wrong.add(std::string("not-a-float")).add(0.8f).add(std::int32_t{1});
    processor.submit_osc_for_test(wrong);
    const auto& rejected = processor.published_control_for_test();
    REQUIRE(rejected.sequence == 0);
    REQUIRE(rejected.status == pulp::runtime::ExternalInputStatus::unavailable);

    pulp::osc::Message valid("/pulp/control");
    valid.add(0.4f).add(0.8f).add(std::int32_t{1});
    processor.submit_osc_for_test(valid);
    const auto& published = processor.published_control_for_test();
    REQUIRE(published.sequence == 1);
    REQUIRE(published.status == pulp::runtime::ExternalInputStatus::valid);
    REQUIRE(published.values[0] == 0.4f);
}

TEST_CASE("reserved MIDI packet rejects invalid status and data bytes", "[external-input][midi]") {
    using pulp::examples::cv_control::decode_midi_control;
    REQUIRE_FALSE(decode_midi_control(0xee, 0, 0, 1).has_value());
    REQUIRE_FALSE(decode_midi_control(0xef, 128, 0, 1).has_value());
    REQUIRE_FALSE(decode_midi_control(0xef, 0, 128, 1).has_value());

    const auto valid = decode_midi_control(0xef, 0x20, 0x40, 1);
    REQUIRE(valid.has_value());
    REQUIRE(valid->valid);
}
