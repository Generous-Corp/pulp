#include <catch2/catch_test_macros.hpp>

#include <pulp/format/app_updates.hpp>
#include <pulp/format/detail/standalone_updater.hpp>

using pulp::format::detail::add_standalone_updater_menu_command;
using pulp::format::detail::make_standalone_update_service;
using pulp::format::detail::make_stub_update_service;
using pulp::format::detail::plan_standalone_updater;
using pulp::format::detail::probe_standalone_updater_environment;
using pulp::format::detail::StandaloneUpdaterBackend;
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
    CHECK(plan.backend == StandaloneUpdaterBackend::sparkle);
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
        CHECK(plan.backend == StandaloneUpdaterBackend::none);
        CHECK(make_standalone_update_service(plan) == nullptr);
    }
}

TEST_CASE("The stub backend wires the menu and Settings in a build without Sparkle",
          "[standalone][updater]") {
    // A development identity that embeds no Sparkle and declares no feed.
    StandaloneUpdaterEnvironment dev;
    dev.override_value = "stub";
    const auto plan = plan_standalone_updater(dev);
    CHECK(plan.offer_menu_command);
    CHECK_FALSE(plan.start_at_launch);
    CHECK(plan.backend == StandaloneUpdaterBackend::stub);

    auto service = make_standalone_update_service(plan);
    REQUIRE(service != nullptr);
    auto status = service->status();
    CHECK(status.stub);
    CHECK(status.can_check_now);
    CHECK(status.last_check_unix_seconds == 0);
    CHECK(service->check_for_updates());
    CHECK(service->status().last_check_unix_seconds > 0);
    CHECK(service->set_automatic_checks(true));
    CHECK(service->status().automatic_checks);

    // Headless still wins: a screenshot run never offers updates.
    dev.headless = true;
    CHECK(plan_standalone_updater(dev).backend == StandaloneUpdaterBackend::none);
}

TEST_CASE("Check for Updates sits directly under About and goes through the service",
          "[standalone][updater]") {
    using Section = pulp::view::WindowOptions::MenuCommand::AppMenuSection;
    pulp::view::WindowOptions options;
    options.menu_commands.push_back({.menu = {}, .title = "Settings\xE2\x80\xA6",
                                     .action = [] {}});
    options.menu_commands.push_back({.menu = "Window", .title = "Keyboard",
                                     .action = [] {}});

    add_standalone_updater_menu_command(options, StandaloneUpdaterPlan{});
    REQUIRE(options.menu_commands.size() == 2); // negative control: not offered

    add_standalone_updater_menu_command(
        options, {.offer_menu_command = true, .backend = StandaloneUpdaterBackend::stub});
    REQUIRE(options.menu_commands.size() == 3);
    const auto& command = options.menu_commands.back();
    CHECK(command.menu.empty()); // the application menu
    CHECK(command.title == "Check for Updates\xE2\x80\xA6");
    CHECK(command.app_menu_section == Section::after_about);
    // Settings… keeps its own place in the app-command group.
    CHECK(options.menu_commands.front().app_menu_section == Section::commands);

    // The item drives whatever service is installed.
    auto stub = make_stub_update_service("Fixture", "1.0.0");
    pulp::format::set_app_update_service(stub);
    REQUIRE(static_cast<bool>(command.action));
    command.action();
    CHECK(stub->status().last_check_unix_seconds > 0);
    pulp::format::set_app_update_service(nullptr);
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
