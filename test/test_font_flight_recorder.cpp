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

TEST_CASE("FlightRecorder: capacity=0 disables recording", "[font][diagnostics]") {
    auto& r = FontFlightRecorder::instance();
    r.clear();
    r.set_capacity(0);

    r.record_fallback({"x", "x", 0u, 0u, 0u});
    REQUIRE(r.size() == 0u);

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
