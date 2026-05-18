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

TEST_CASE("Analytics flush reaches destinations even when disabled", "[runtime][analytics][issue-641]") {
    auto events = std::make_shared<std::vector<AnalyticsEvent>>();
    auto flushes = std::make_shared<int>(0);

    auto& a = Analytics::instance();
    a.add_destination(std::make_unique<RecordingDestination>(events, flushes));

    a.set_enabled(false);
    a.flush();
    REQUIRE(*flushes == 1);
    REQUIRE(events->empty());

    a.set_enabled(true);
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
    event.properties = {
        {"key\"name", "value\\path\tend"},
        {"return", "line\rend"},
    };
    dest.log_event(event);
    dest.flush();

    std::ifstream f(tmp.path());
    std::string line;
    REQUIRE(std::getline(f, line));
    REQUIRE(line.find("\"event\":\"quote\\\"and\\nline\"") != std::string::npos);
    REQUIRE(line.find("\"key\\\"name\":\"value\\\\path\\tend\"") != std::string::npos);
    REQUIRE(line.find("\"return\":\"line\\rend\"") != std::string::npos);
    REQUIRE(line.find("\",\"return\"") != std::string::npos);
}

TEST_CASE("FileAnalyticsDestination escapes each special JSON character",
          "[runtime][analytics][coverage][issue-656]") {
    struct Case {
        std::string input;
        std::string expected;
    };

    const Case cases[] = {
        {"back\\slash", "back\\\\slash"},
        {"double\"quote", "double\\\"quote"},
        {"new\nline", "new\\nline"},
        {"carriage\rreturn", "carriage\\rreturn"},
        {"tab\tchar", "tab\\tchar"},
    };

    for (const auto& c : cases) {
        TemporaryFile tmp(".jsonl");
        FileAnalyticsDestination dest(tmp.path_string());

        AnalyticsEvent event;
        event.name = c.input;
        event.timestamp = 1.0;
        dest.log_event(event);
        dest.flush();

        std::ifstream f(tmp.path());
        std::string line;
        REQUIRE(std::getline(f, line));
        REQUIRE(line.find("\"event\":\"" + c.expected + "\"") != std::string::npos);
    }
}

TEST_CASE("FileAnalyticsDestination escapes special JSON property keys and values",
          "[runtime][analytics][coverage][issue-656]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    AnalyticsEvent event;
    event.name = "props";
    event.timestamp = 1.0;
    event.properties = {
        {"slash\\key", "quote\"value"},
        {"line\nkey", "return\rvalue"},
        {"tab\tkey", "tab\tvalue"},
    };
    dest.log_event(event);
    dest.flush();

    std::ifstream f(tmp.path());
    std::string line;
    REQUIRE(std::getline(f, line));
    REQUIRE(line.find("\"slash\\\\key\":\"quote\\\"value\"") != std::string::npos);
    REQUIRE(line.find("\"line\\nkey\":\"return\\rvalue\"") != std::string::npos);
    REQUIRE(line.find("\"tab\\tkey\":\"tab\\tvalue\"") != std::string::npos);
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

TEST_CASE("FileAnalyticsDestination omits props object for empty properties",
          "[runtime][analytics][coverage][phase3-large]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    AnalyticsEvent event;
    event.name = "no_props";
    event.timestamp = 2.5;
    dest.log_event(event);
    dest.flush();

    std::ifstream f(tmp.path());
    std::string line;
    REQUIRE(std::getline(f, line));
    REQUIRE(line.find("\"event\":\"no_props\"") != std::string::npos);
    REQUIRE(line.find("\"time\":2.5") != std::string::npos);
    REQUIRE(line.find("\"props\"") == std::string::npos);
}

TEST_CASE("FileAnalyticsDestination buffers below auto flush threshold",
          "[runtime][analytics][coverage][phase3-large]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    for (int i = 0; i < 99; ++i) {
        AnalyticsEvent event;
        event.name = "buffered";
        dest.log_event(event);
    }

    {
        std::ifstream before(tmp.path());
        std::string line;
        REQUIRE_FALSE(std::getline(before, line));
    }

    dest.flush();
    std::ifstream after(tmp.path());
    int lines = 0;
    std::string line;
    while (std::getline(after, line))
        ++lines;
    REQUIRE(lines == 99);
}

TEST_CASE("FileAnalyticsDestination second flush does not duplicate events",
          "[runtime][analytics][coverage][phase3-large]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    AnalyticsEvent event;
    event.name = "once";
    dest.log_event(event);
    dest.flush();
    dest.flush();

    std::ifstream f(tmp.path());
    int lines = 0;
    std::string line;
    while (std::getline(f, line))
        ++lines;
    REQUIRE(lines == 1);
}

TEST_CASE("FileAnalyticsDestination retains buffered events after open failure",
          "[runtime][analytics][coverage][phase3-large]") {
    auto dir = std::filesystem::temp_directory_path() / "pulp_analytics_dir_dest";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);

    FileAnalyticsDestination dest(dir.string());
    AnalyticsEvent event;
    event.name = "queued";
    event.timestamp = 4.0;
    dest.log_event(event);
    dest.flush();

    REQUIRE(std::filesystem::is_directory(dir));

    std::filesystem::remove_all(dir);
    dest.flush();

    std::ifstream f(dir);
    std::string line;
    REQUIRE(std::getline(f, line));
    REQUIRE(line.find("\"event\":\"queued\"") != std::string::npos);

    std::filesystem::remove(dir);
}

TEST_CASE("FileAnalyticsDestination empty flush does not create output", "[runtime][analytics][issue-641]") {
    TemporaryFile tmp(".jsonl");
    auto path = tmp.path();
    std::filesystem::remove(path);

    FileAnalyticsDestination dest(tmp.path_string());
    dest.flush();

    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("FileAnalyticsDestination writes property map in key order", "[runtime][analytics][issue-641]") {
    TemporaryFile tmp(".jsonl");
    FileAnalyticsDestination dest(tmp.path_string());

    AnalyticsEvent event;
    event.name = "ordered";
    event.properties = {{"z", "last"}, {"a", "first"}};
    dest.log_event(event);
    dest.flush();

    std::ifstream f(tmp.path());
    std::string line;
    REQUIRE(std::getline(f, line));
    REQUIRE(line.find("\"a\":\"first\"") < line.find("\"z\":\"last\""));
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

TEST_CASE("WidgetTracker forwards expected event names and properties",
          "[runtime][analytics][issue-641]") {
    auto events = std::make_shared<std::vector<AnalyticsEvent>>();
    auto flushes = std::make_shared<int>(0);

    auto& a = Analytics::instance();
    a.set_enabled(true);
    a.add_destination(std::make_unique<RecordingDestination>(events, flushes));

    WidgetTracker::track_click("bypass");
    WidgetTracker::track_value_change("gain", "-6");
    WidgetTracker::track_preset_select("Init");

    REQUIRE(events->size() == 3);
    REQUIRE((*events)[0].name == "widget_click");
    REQUIRE((*events)[0].properties.at("widget") == "bypass");
    REQUIRE((*events)[1].name == "value_change");
    REQUIRE((*events)[1].properties.at("widget") == "gain");
    REQUIRE((*events)[1].properties.at("value") == "-6");
    REQUIRE((*events)[2].name == "preset_select");
    REQUIRE((*events)[2].properties.at("preset") == "Init");
}

TEST_CASE("Analytics forwards simple log with empty property map",
          "[runtime][analytics][coverage][phase3-large]") {
    auto events = std::make_shared<std::vector<AnalyticsEvent>>();
    auto flushes = std::make_shared<int>(0);

    auto& a = Analytics::instance();
    a.set_enabled(true);
    a.add_destination(std::make_unique<RecordingDestination>(events, flushes));

    a.log("plain");
    REQUIRE(events->size() == 1);
    REQUIRE(events->back().name == "plain");
    REQUIRE(events->back().properties.empty());
}

TEST_CASE("Analytics disabled widget tracker calls skip destinations",
          "[runtime][analytics][coverage][phase3-large]") {
    auto events = std::make_shared<std::vector<AnalyticsEvent>>();
    auto flushes = std::make_shared<int>(0);

    auto& a = Analytics::instance();
    a.add_destination(std::make_unique<RecordingDestination>(events, flushes));

    a.set_enabled(false);
    WidgetTracker::track_click("disabled");
    WidgetTracker::track_value_change("disabled", "0");
    WidgetTracker::track_preset_select("disabled");

    REQUIRE(events->empty());
    a.set_enabled(true);
}

TEST_CASE("WidgetTracker preserves empty identifiers and values",
          "[runtime][analytics][coverage][phase3-large]") {
    auto events = std::make_shared<std::vector<AnalyticsEvent>>();
    auto flushes = std::make_shared<int>(0);

    auto& a = Analytics::instance();
    a.set_enabled(true);
    a.add_destination(std::make_unique<RecordingDestination>(events, flushes));

    WidgetTracker::track_click("");
    WidgetTracker::track_value_change("", "");
    WidgetTracker::track_preset_select("");

    REQUIRE(events->size() == 3);
    REQUIRE((*events)[0].properties.at("widget").empty());
    REQUIRE((*events)[1].properties.at("widget").empty());
    REQUIRE((*events)[1].properties.at("value").empty());
    REQUIRE((*events)[2].properties.at("preset").empty());
}
