// Unit tests for the cross-platform live-region announcement API
// (workstream 04 slice 4.3). Verifies the sink plumbing — platform-specific
// backends (NSAccessibilityPostNotification, UIA, AT-SPI) get covered as
// they land in slices 4.1 / 4.2.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/accessibility.hpp>

#include <string>
#include <vector>

using namespace pulp::view;

namespace {

struct Captured {
    std::string text;
    AnnouncementPriority priority;
};

} // namespace

TEST_CASE("announce_accessibility with no sink is a safe no-op",
          "[a11y][announce]") {
    // Default state: no sink installed. Should not throw or crash.
    set_announcement_sink(nullptr);
    announce_accessibility("this goes to the log");
    announce_accessibility("this too", AnnouncementPriority::Assertive);
    SUCCEED("announcement with no sink returned cleanly");
}

TEST_CASE("set_announcement_sink captures announcements",
          "[a11y][announce]") {
    std::vector<Captured> calls;
    set_announcement_sink([&](std::string_view t, AnnouncementPriority p) {
        calls.push_back({std::string(t), p});
    });

    announce_accessibility("polite default");
    announce_accessibility("interrupt!", AnnouncementPriority::Assertive);

    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].text == "polite default");
    REQUIRE(calls[0].priority == AnnouncementPriority::Polite);
    REQUIRE(calls[1].text == "interrupt!");
    REQUIRE(calls[1].priority == AnnouncementPriority::Assertive);

    // Tear down: restore default.
    set_announcement_sink(nullptr);
    announce_accessibility("post-detach");
    REQUIRE(calls.size() == 2); // sink detached — no new calls
}

TEST_CASE("announcement sink can be replaced",
          "[a11y][announce]") {
    int first_count = 0;
    int second_count = 0;
    set_announcement_sink([&](std::string_view, AnnouncementPriority) {
        ++first_count;
    });
    announce_accessibility("a");
    announce_accessibility("b");

    set_announcement_sink([&](std::string_view, AnnouncementPriority) {
        ++second_count;
    });
    announce_accessibility("c");

    REQUIRE(first_count == 2);
    REQUIRE(second_count == 1);

    set_announcement_sink(nullptr);
}
