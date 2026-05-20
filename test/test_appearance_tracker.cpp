#include <catch2/catch_test_macros.hpp>
#include <pulp/view/appearance_tracker.hpp>
#include <vector>

using namespace pulp::view;

TEST_CASE("AppearanceTracker default appearance", "[view][appearance]") {
    AppearanceTracker tracker;
    // Should return some valid appearance (platform-dependent)
    auto appearance = tracker.current_appearance();
    REQUIRE((appearance == Appearance::light || appearance == Appearance::dark));
}

TEST_CASE("AppearanceTracker lock overrides system", "[view][appearance]") {
    AppearanceTracker tracker;

    tracker.lock(Appearance::light);
    REQUIRE(tracker.is_locked());
    REQUIRE(tracker.current_appearance() == Appearance::light);

    tracker.lock(Appearance::dark);
    REQUIRE(tracker.current_appearance() == Appearance::dark);

    tracker.unlock();
    REQUIRE_FALSE(tracker.is_locked());
}

TEST_CASE("AppearanceTracker poll returns false when locked", "[view][appearance]") {
    AppearanceTracker tracker;
    tracker.lock(Appearance::dark);
    REQUIRE_FALSE(tracker.poll());
}

TEST_CASE("AppearanceTracker callback fires on lock", "[view][appearance]") {
    AppearanceTracker tracker;
    Appearance received = Appearance::dark;

    tracker.on_appearance_changed([&](Appearance a) { received = a; });
    tracker.lock(Appearance::light);

    REQUIRE(received == Appearance::light);
}

TEST_CASE("AppearanceTracker replacing callback uses latest handler only",
          "[view][appearance][coverage][phase3]") {
    AppearanceTracker tracker;
    int first_calls = 0;
    std::vector<Appearance> latest;

    tracker.on_appearance_changed([&](Appearance) { ++first_calls; });
    tracker.on_appearance_changed([&](Appearance a) { latest.push_back(a); });

    tracker.lock(Appearance::light);
    tracker.lock(Appearance::dark);

    REQUIRE(first_calls == 0);
    REQUIRE(latest == std::vector<Appearance>{Appearance::light, Appearance::dark});
}

TEST_CASE("AppearanceTracker callbacks follow repeated locks and locked poll no-op",
          "[view][appearance][coverage][issue-493]") {
    AppearanceTracker tracker;
    std::vector<Appearance> received;

    tracker.on_appearance_changed([&](Appearance a) { received.push_back(a); });
    tracker.lock(Appearance::light);
    tracker.lock(Appearance::dark);

    REQUIRE(tracker.is_locked());
    REQUIRE(tracker.locked_appearance() == Appearance::dark);
    REQUIRE(tracker.current_appearance() == Appearance::dark);
    REQUIRE_FALSE(tracker.poll());
    REQUIRE(received == std::vector<Appearance>{Appearance::light, Appearance::dark});

    const auto count_before_unlock = received.size();
    tracker.unlock();
    REQUIRE_FALSE(tracker.is_locked());
    REQUIRE((tracker.current_appearance() == Appearance::light ||
             tracker.current_appearance() == Appearance::dark));

    if (received.size() > count_before_unlock)
        REQUIRE(received.back() == tracker.current_appearance());
}

// ── ThemeManager ────────────────────────────────────────────────────────────

TEST_CASE("ThemeManager default themes", "[view][appearance]") {
    ThemeManager mgr;
    // Should have a theme
    auto& theme = mgr.active_theme();
    REQUIRE(theme.color("bg.primary").has_value());
}

TEST_CASE("ThemeManager lock appearance", "[view][appearance]") {
    ThemeManager mgr;
    auto light = Theme::light();
    auto dark = Theme::dark();
    mgr.set_theme_pair(light, dark);

    mgr.lock_appearance(Appearance::light);
    REQUIRE(mgr.is_locked());
    auto light_bg = mgr.active_theme().color("bg.primary").value();

    mgr.lock_appearance(Appearance::dark);
    auto dark_bg = mgr.active_theme().color("bg.primary").value();

    REQUIRE_FALSE(light_bg == dark_bg);
}

TEST_CASE("ThemeManager lock specific theme", "[view][appearance]") {
    ThemeManager mgr;

    Theme custom;
    custom.colors["bg.primary"] = color_from_hex(0xFF0000);
    mgr.lock_theme(custom);

    REQUIRE(mgr.is_locked());
    REQUIRE(mgr.active_theme().color("bg.primary")->r8() == 0xFF);
    REQUIRE(mgr.active_theme().color("bg.primary")->g8() == 0x00);
}

TEST_CASE("ThemeManager callback fires on theme change", "[view][appearance]") {
    ThemeManager mgr;
    bool called = false;

    mgr.on_theme_changed([&](const Theme&) { called = true; });
    mgr.lock_appearance(Appearance::light);

    REQUIRE(called);
}

TEST_CASE("ThemeManager replacing callback uses latest handler only",
          "[view][appearance][coverage][phase3]") {
    ThemeManager mgr;
    int first_calls = 0;
    std::vector<uint8_t> latest_red;

    Theme light;
    light.colors["bg.primary"] = color_from_hex(0x110000);
    Theme dark;
    dark.colors["bg.primary"] = color_from_hex(0x220000);
    mgr.set_theme_pair(light, dark);

    mgr.on_theme_changed([&](const Theme&) { ++first_calls; });
    mgr.on_theme_changed([&](const Theme& theme) {
        latest_red.push_back(theme.color("bg.primary")->r8());
    });

    mgr.lock_appearance(Appearance::light);
    mgr.lock_appearance(Appearance::dark);

    REQUIRE(first_calls == 0);
    REQUIRE(latest_red == std::vector<uint8_t>{0x11, 0x22});
}

TEST_CASE("ThemeManager callbacks cover locked theme poll and unlock",
          "[view][appearance][coverage][issue-493]") {
    ThemeManager mgr;

    Theme light;
    light.colors["bg.primary"] = color_from_hex(0x110000);
    Theme dark;
    dark.colors["bg.primary"] = color_from_hex(0x220000);
    Theme locked;
    locked.colors["bg.primary"] = color_from_hex(0x330000);
    mgr.set_theme_pair(light, dark);

    std::vector<uint8_t> callback_red;
    mgr.on_theme_changed([&](const Theme& theme) {
        callback_red.push_back(theme.color("bg.primary")->r8());
    });

    mgr.lock_theme(locked);
    REQUIRE(mgr.is_locked());
    REQUIRE(mgr.active_theme().color("bg.primary")->r8() == 0x33);
    REQUIRE(callback_red == std::vector<uint8_t>{0x33});
    REQUIRE_FALSE(mgr.poll());
    REQUIRE(callback_red == std::vector<uint8_t>{0x33});

    mgr.lock_appearance(Appearance::light);
    REQUIRE(mgr.is_locked());
    REQUIRE(mgr.active_theme().color("bg.primary")->r8() == 0x11);

    mgr.lock_appearance(Appearance::dark);
    REQUIRE(mgr.active_theme().color("bg.primary")->r8() == 0x22);

    mgr.unlock();
    REQUIRE_FALSE(mgr.is_locked());
    REQUIRE(callback_red.size() == 4);
    REQUIRE(callback_red[0] == 0x33);
    REQUIRE(callback_red[1] == 0x11);
    REQUIRE(callback_red[2] == 0x22);
}
