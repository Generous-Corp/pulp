#include <catch2/catch_test_macros.hpp>
#include <pulp/view/appearance_tracker.hpp>

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
