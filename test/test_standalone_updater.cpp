#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/standalone_updater.hpp>

using pulp::format::detail::add_standalone_updater_menu_command;
using pulp::format::detail::plan_standalone_updater;
using pulp::format::detail::probe_standalone_updater_environment;
using pulp::format::detail::StandaloneUpdaterEnvironment;
using pulp::format::detail::StandaloneUpdaterPlan;

namespace {

StandaloneUpdaterEnvironment shipping_app() {
    StandaloneUpdaterEnvironment env;
    env.bundle_declares_feed = true;
    env.updater_framework_loaded = true;
    env.developer_id_signed = true;
    return env;
}

} // namespace

TEST_CASE("A Developer ID app that embeds Sparkle offers updates and checks at launch",
          "[standalone][updater]") {
    const auto plan = plan_standalone_updater(shipping_app());
    CHECK(plan.offer_menu_command);
    CHECK(plan.start_at_launch);
}

TEST_CASE("A development build offers the menu item but never checks on its own",
          "[standalone][updater]") {
    auto env = shipping_app();
    env.developer_id_signed = false;
    const auto plan = plan_standalone_updater(env);
    CHECK(plan.offer_menu_command);
    CHECK_FALSE(plan.start_at_launch);

    // Practising against a local feed: an explicit opt-in starts checks.
    for (const char* on : {"1", "on", "ON", "true"}) {
        env.override_value = on;
        CHECK(plan_standalone_updater(env).start_at_launch);
    }
}

TEST_CASE("The updater stays off without a feed, without Sparkle, headless, or when disabled",
          "[standalone][updater]") {
    auto no_feed = shipping_app();
    no_feed.bundle_declares_feed = false;
    auto no_sparkle = shipping_app();
    no_sparkle.updater_framework_loaded = false;
    auto headless = shipping_app();
    headless.headless = true;
    auto disabled = shipping_app();
    disabled.override_value = "off";
    auto disabled_zero = shipping_app();
    disabled_zero.override_value = "0";

    for (const auto& env : {no_feed, no_sparkle, headless, disabled, disabled_zero}) {
        const auto plan = plan_standalone_updater(env);
        CHECK_FALSE(plan.offer_menu_command);
        CHECK_FALSE(plan.start_at_launch);
    }
}

TEST_CASE("Check for Updates goes first in the app menu, ahead of Settings",
          "[standalone][updater]") {
    pulp::view::WindowOptions options;
    options.menu_commands.push_back({.menu = {}, .title = "Settings\xE2\x80\xA6",
                                     .action = [] {}});
    options.menu_commands.push_back({.menu = "Window", .title = "Keyboard",
                                     .action = [] {}});

    add_standalone_updater_menu_command(options, StandaloneUpdaterPlan{});
    REQUIRE(options.menu_commands.size() == 2);

    add_standalone_updater_menu_command(options, {.offer_menu_command = true});
    REQUIRE(options.menu_commands.size() == 3);
    const auto& first = options.menu_commands.front();
    CHECK(first.menu.empty());  // the application menu
    CHECK(first.title == "Check for Updates\xE2\x80\xA6");
    CHECK(static_cast<bool>(first.action));
    CHECK(options.menu_commands[1].title == "Settings\xE2\x80\xA6");
}

TEST_CASE("A process with no app bundle and no Sparkle probes as not updatable",
          "[standalone][updater]") {
    // The test binary is a bare executable: no Info.plist SUFeedURL, and no
    // Sparkle.framework in a Contents/Frameworks of its own.
    const auto env = probe_standalone_updater_environment(/*headless=*/false);
    CHECK_FALSE(env.bundle_declares_feed);
    CHECK_FALSE(env.updater_framework_loaded);
    CHECK_FALSE(plan_standalone_updater(env).offer_menu_command);
    CHECK(probe_standalone_updater_environment(true).headless);
}
