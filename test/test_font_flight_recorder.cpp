// test_font_flight_recorder.cpp — Pulp #2163, font v2 Slice 2.2.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_flight_recorder.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkTypeface.h"
#endif

#include <string>

using namespace pulp::canvas;

TEST_CASE("FlightRecorder: default capacity is non-zero", "[font][diagnostics][issue-2163]") {
    auto& r = FontFlightRecorder::instance();
    REQUIRE(r.capacity() > 0u);
}

TEST_CASE("FlightRecorder: record + drain round-trip", "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    REQUIRE(r.size() == 0u);

    r.record_fallback({"Inter", "Inter", 2u, 0u, 0u});
    r.record_fallback({"JetBrains Mono", "JetBrains Mono", 3u, 0u, 0u});
    REQUIRE(r.size() == 2u);

    auto out = r.drain();
    REQUIRE(out.size() == 2u);
    REQUIRE(r.size() == 0u);
    REQUIRE(out[0].requested_family == "Inter");
    REQUIRE(out[1].requested_family == "JetBrains Mono");
    REQUIRE(out[0].sequence < out[1].sequence);  // monotonic
}

TEST_CASE("FlightRecorder: capacity overflow drops oldest", "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(3);

    r.record_fallback({"a", "a", 0u, 0u, 0u});
    r.record_fallback({"b", "b", 0u, 0u, 0u});
    r.record_fallback({"c", "c", 0u, 0u, 0u});
    r.record_fallback({"d", "d", 0u, 0u, 0u});  // pushes 'a' off

    auto out = r.snapshot();
    REQUIRE(out.size() == 3u);
    REQUIRE(out[0].requested_family == "b");
    REQUIRE(out[2].requested_family == "d");

    r.set_capacity(1024);  // restore default
    r.clear();
}

TEST_CASE("FlightRecorder: snapshot is non-destructive and ordered",
          "[font][diagnostics][coverage]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(4);

    r.record_fallback({"first", "selected-first", 1u, 11u, 0u});
    r.record_fallback({"second", "selected-second", 2u, 12u, 0u});

    auto first_snapshot = r.snapshot();
    REQUIRE(first_snapshot.size() == 2u);
    REQUIRE(first_snapshot[0].requested_family == "first");
    REQUIRE(first_snapshot[1].requested_family == "second");
    REQUIRE(first_snapshot[0].sequence < first_snapshot[1].sequence);
    REQUIRE(r.size() == 2u);

    auto second_snapshot = r.snapshot();
    REQUIRE(second_snapshot.size() == 2u);
    REQUIRE(second_snapshot[0].sequence == first_snapshot[0].sequence);
    REQUIRE(second_snapshot[1].sequence == first_snapshot[1].sequence);

    auto drained = r.drain();
    REQUIRE(drained.size() == 2u);
    REQUIRE(r.size() == 0u);

    r.set_capacity(1024);
}

TEST_CASE("FlightRecorder: shrinking capacity trims oldest records",
          "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(4);

    r.record_fallback({"a", "a", 0u, 0u, 0u});
    r.record_fallback({"b", "b", 0u, 0u, 0u});
    r.record_fallback({"c", "c", 0u, 0u, 0u});
    r.record_fallback({"d", "d", 0u, 0u, 0u});

    r.set_capacity(2);
    auto out = r.snapshot();
    REQUIRE(out.size() == 2u);
    REQUIRE(out[0].requested_family == "c");
    REQUIRE(out[1].requested_family == "d");

    r.set_capacity(1024);
    r.clear();
}

TEST_CASE("FlightRecorder: capacity=0 trims existing records immediately",
          "[font][diagnostics][coverage]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(3);

    r.record_fallback({"a", "a", 0u, 0u, 0u});
    r.record_fallback({"b", "b", 0u, 0u, 0u});
    REQUIRE(r.size() == 2u);

    r.set_capacity(0);
    REQUIRE(r.capacity() == 0u);
    REQUIRE(r.size() == 0u);
    REQUIRE(r.snapshot().empty());

    r.set_capacity(1024);
}

TEST_CASE("FlightRecorder: capacity=0 disables recording", "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(0);

    r.record_fallback({"x", "x", 0u, 0u, 0u});
    REQUIRE(r.size() == 0u);

    r.set_capacity(1024);
}

TEST_CASE("FlightRecorder: disabled recording still advances sequence",
          "[font][diagnostics][coverage]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(1);

    r.record_fallback({"before", "before", 0u, 0u, 0u});
    const auto before = r.drain().front().sequence;

    r.set_capacity(0);
    r.record_fallback({"suppressed", "suppressed", 0u, 0u, 0u});
    REQUIRE(r.size() == 0u);

    r.set_capacity(1);
    r.record_fallback({"after", "after", 0u, 0u, 0u});
    const auto after = r.drain().front().sequence;

    REQUIRE(after > before + 1u);

    r.set_capacity(1024);
}

TEST_CASE("FlightRecorder: clear preserves monotonic sequence",
          "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(1024);

    r.record_fallback({"first", "first", 0u, 0u, 0u});
    const auto before = r.drain().front().sequence;
    r.clear();
    r.record_fallback({"second", "second", 0u, 0u, 0u});
    const auto after = r.drain().front().sequence;

    REQUIRE(after > before);
}

TEST_CASE("FlightRecorder: JSON drain is well-formed", "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.record_fallback({"Inter", "Inter", 2u, 7u, 0u});
    std::string json = flight_recorder_drain_json();

    REQUIRE(json.find("\"records\"") != std::string::npos);
    REQUIRE(json.find("\"requested_family\":\"Inter\"") != std::string::npos);
    REQUIRE(json.find("\"selected_family\":\"Inter\"") != std::string::npos);
    REQUIRE(json.find("\"origin\":2") != std::string::npos);
    REQUIRE(json.find("\"generation\":7") != std::string::npos);
}

TEST_CASE("FlightRecorder: JSON drain preserves order and sequence fields",
          "[font][diagnostics][coverage]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(4);

    r.record_fallback({"alpha", "A", 1u, 21u, 0u});
    r.record_fallback({"beta", "B", 2u, 22u, 0u});
    const auto expected = r.snapshot();
    const std::string json = flight_recorder_drain_json();

    const auto alpha_pos = json.find("\"requested_family\":\"alpha\"");
    const auto beta_pos = json.find("\"requested_family\":\"beta\"");
    REQUIRE(alpha_pos != std::string::npos);
    REQUIRE(beta_pos != std::string::npos);
    REQUIRE(alpha_pos < beta_pos);
    REQUIRE(json.find("\"sequence\":" + std::to_string(expected[0].sequence)) != std::string::npos);
    REQUIRE(json.find("\"sequence\":" + std::to_string(expected[1].sequence)) != std::string::npos);
    REQUIRE(r.size() == 0u);

    r.set_capacity(1024);
}

TEST_CASE("FlightRecorder: JSON drain escapes strings and handles empty records",
          "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    REQUIRE(flight_recorder_drain_json() == "{\"records\":[]}");

    r.record_fallback({"Quote\"Slash\\Tab\tLine\nReturn\r",
                       "Selected",
                       1u,
                       9u,
                       0u});
    const std::string json = flight_recorder_drain_json();

    REQUIRE(json.find("Quote\\\"Slash\\\\Tab\\tLine\\nReturn\\r") != std::string::npos);
    REQUIRE(json.find("\"selected_family\":\"Selected\"") != std::string::npos);
    REQUIRE(r.size() == 0u);
}

TEST_CASE("FlightRecorder: FontResolver pushes events on resolve",
          "[font][diagnostics][skia]") {
#ifdef PULP_HAS_SKIA
    auto& r = FontFlightRecorder::instance();
    r.clear();
    REQUIRE(r.size() == 0u);

    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (resolved.resolved()) {
        // At least one record should have been pushed.
        REQUIRE(r.size() >= 1u);
        auto out = r.drain();
        REQUIRE(out.front().requested_family == "Inter");
    }
#else
    SUCCEED("Skia not compiled");
#endif
}
