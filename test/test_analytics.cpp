#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/analytics.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <fstream>

using namespace pulp::runtime;

TEST_CASE("Analytics log event increments count", "[runtime][analytics]") {
    // Use a fresh destination to test
    auto& a = Analytics::instance();
    a.set_enabled(true);
    uint64_t before = a.event_count();

    a.log("test_event");
    REQUIRE(a.event_count() == before + 1);
}

TEST_CASE("Analytics disabled doesn't log", "[runtime][analytics]") {
    auto& a = Analytics::instance();
    uint64_t before = a.event_count();

    a.set_enabled(false);
    a.log("should_not_count");
    REQUIRE(a.event_count() == before);

    a.set_enabled(true);
}

TEST_CASE("Analytics log with properties", "[runtime][analytics]") {
    auto& a = Analytics::instance();
    uint64_t before = a.event_count();

    a.log_event("user_action", {{"button", "play"}, {"state", "on"}});
    REQUIRE(a.event_count() == before + 1);
}

TEST_CASE("FileAnalyticsDestination writes JSON", "[runtime][analytics]") {
    TemporaryFile tmp(".jsonl");

    {
        auto dest = std::make_unique<FileAnalyticsDestination>(tmp.path_string());
        AnalyticsEvent event;
        event.name = "test";
        event.timestamp = 1234567890.0;
        event.properties = {{"key", "value"}};
        dest->log_event(event);
        dest->flush();
    }

    std::ifstream f(tmp.path());
    std::string line;
    REQUIRE(std::getline(f, line));
    REQUIRE(line.find("\"event\":\"test\"") != std::string::npos);
    REQUIRE(line.find("\"key\":\"value\"") != std::string::npos);
}

TEST_CASE("WidgetTracker logs events", "[runtime][analytics]") {
    auto& a = Analytics::instance();
    a.set_enabled(true);
    uint64_t before = a.event_count();

    WidgetTracker::track_click("gain_knob");
    WidgetTracker::track_value_change("freq", "440");
    WidgetTracker::track_preset_select("Default");

    REQUIRE(a.event_count() == before + 3);
}
