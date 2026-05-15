#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/analytics.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <fstream>
#include <filesystem>
#include <memory>
#include <vector>

using namespace pulp::runtime;

namespace {

struct RecordingDestination final : AnalyticsDestination {
    explicit RecordingDestination(std::shared_ptr<std::vector<AnalyticsEvent>> events_in,
                                  std::shared_ptr<int> flushes_in)
        : events(std::move(events_in)), flushes(std::move(flushes_in)) {}

    void log_event(const AnalyticsEvent& event) override {
        events->push_back(event);
    }

    void flush() override {
        ++(*flushes);
    }

    std::shared_ptr<std::vector<AnalyticsEvent>> events;
    std::shared_ptr<int> flushes;
};

}  // namespace

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

TEST_CASE("Analytics forwards event details and flushes destinations", "[runtime][analytics]") {
    auto events = std::make_shared<std::vector<AnalyticsEvent>>();
    auto flushes = std::make_shared<int>(0);

    auto& a = Analytics::instance();
    a.set_enabled(true);
    a.add_destination(std::make_unique<RecordingDestination>(events, flushes));

    a.log_event("transport", {{"state", "playing"}});
    REQUIRE(events->size() == 1);
    REQUIRE(events->back().name == "transport");
    REQUIRE(events->back().properties.at("state") == "playing");
    REQUIRE(events->back().timestamp > 0.0);

    a.flush();
    REQUIRE(*flushes == 1);
}

TEST_CASE("Analytics disabled skips destinations", "[runtime][analytics]") {
    auto events = std::make_shared<std::vector<AnalyticsEvent>>();
    auto flushes = std::make_shared<int>(0);

    auto& a = Analytics::instance();
    a.add_destination(std::make_unique<RecordingDestination>(events, flushes));

    a.set_enabled(false);
    a.log_event("disabled_event", {{"ignored", "true"}});
    REQUIRE(events->empty());

    a.set_enabled(true);
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

TEST_CASE("FileAnalyticsDestination escapes JSON strings", "[runtime][analytics][coverage][issue-656]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    AnalyticsEvent event;
    event.name = "quote\"and\nline";
    event.timestamp = 1.0;
    event.properties = {{"key\"name", "value\\path\tend"}};
    dest.log_event(event);
    dest.flush();

    std::ifstream f(tmp.path());
    std::string line;
    REQUIRE(std::getline(f, line));
    REQUIRE(line.find("\"event\":\"quote\\\"and\\nline\"") != std::string::npos);
    REQUIRE(line.find("\"key\\\"name\":\"value\\\\path\\tend\"") != std::string::npos);
}

TEST_CASE("FileAnalyticsDestination empty flush does not create output", "[runtime][analytics][coverage][issue-656]") {
    TemporaryFile tmp(".jsonl");
    auto path = tmp.path();
    tmp.release();
    std::filesystem::remove(path);

    FileAnalyticsDestination dest(path.string());
    dest.flush();
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("FileAnalyticsDestination auto flushes at batch threshold", "[runtime][analytics]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    for (int i = 0; i < 100; ++i) {
        AnalyticsEvent event;
        event.name = "batch";
        event.timestamp = static_cast<double>(i);
        dest.log_event(event);
    }

    std::ifstream f(tmp.path());
    int lines = 0;
    std::string line;
    while (std::getline(f, line))
        ++lines;
    REQUIRE(lines == 100);
}

TEST_CASE("FileAnalyticsDestination appends multiple flushes", "[runtime][analytics]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    AnalyticsEvent first;
    first.name = "first";
    dest.log_event(first);
    dest.flush();

    AnalyticsEvent second;
    second.name = "second";
    dest.log_event(second);
    dest.flush();

    std::ifstream f(tmp.path());
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("\"event\":\"first\"") != std::string::npos);
    REQUIRE(content.find("\"event\":\"second\"") != std::string::npos);
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
