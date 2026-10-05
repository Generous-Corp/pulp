// In-app updates as a process-wide service: what an editor reads when the
// standalone installs one, and what every plug-in reads when none is
// installed. Covers the status shape, the generated Settings note, the JS
// bridge messages, the native Settings group, and the standalone Settings
// panel's Updates tab.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/app_updates.hpp>
#include <pulp/format/app_updates_bridge.hpp>
#include <pulp/format/app_updates_settings_view.hpp>
#include <pulp/format/detail/standalone_editor_chrome.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/widgets.hpp>

#include <choc/text/choc_JSON.h>

#include <memory>
#include <string>

using namespace pulp::format;

namespace {

class FakeService final : public AppUpdateService {
  public:
    AppUpdateStatus next;
    int checks = 0;
    int automatic_writes = 0;
    int releases_opened = 0;

    AppUpdateStatus status() const override {
        return next;
    }
    bool check_for_updates() override {
        ++checks;
        next.last_check_unix_seconds = 1'790'000'000;
        return true;
    }
    bool set_automatic_checks(bool on) override {
        ++automatic_writes;
        next.automatic_checks = on;
        return true;
    }
    bool open_releases_page() override {
        ++releases_opened;
        return !next.releases_url.empty();
    }
};

AppUpdateStatus spectr_like() {
    AppUpdateStatus s;
    s.available = true;
    s.can_check_now = true;
    s.app_name = "Spectr";
    s.version = "1.0.7";
    s.build = "1.0.7";
    s.releases_url = "https://github.com/danielraffel/spectr/releases";
    s.feed_host = "github.com";
    s.installer = AppUpdateInstaller::package;
    return s;
}

// Installs a service for one test and always removes it, so a failing CHECK
// never leaks a service into the plug-in (none installed) cases.
struct ScopedService {
    std::shared_ptr<FakeService> service = std::make_shared<FakeService>();
    explicit ScopedService(AppUpdateStatus status) {
        service->next = std::move(status);
        set_app_update_service(service);
    }
    ~ScopedService() {
        set_app_update_service(nullptr);
    }
};

choc::value::Value dispatch(pulp::view::EditorBridge& bridge, const std::string& json) {
    return choc::json::parse(bridge.dispatch_json(json));
}

} // namespace

TEST_CASE("With no service installed (a plug-in) updates are unavailable", "[updates]") {
    set_app_update_service(nullptr);
    const auto status = app_update_status();
    CHECK_FALSE(status.available);
    CHECK_FALSE(check_for_app_updates());
    CHECK_FALSE(set_app_update_automatic_checks(true));
    CHECK(app_update_note(status).empty());
    const auto json = choc::json::parse(app_update_status_json(status));
    CHECK_FALSE(json["available"].getBool());
    CHECK_FALSE(json["canCheckNow"].getBool());
}

TEST_CASE("An installed service is what every surface reads and drives", "[updates]") {
    ScopedService scope(spectr_like());
    auto status = app_update_status();
    CHECK(status.available);
    CHECK(status.app_name == "Spectr");
    CHECK(check_for_app_updates());
    CHECK(scope.service->checks == 1);
    CHECK(set_app_update_automatic_checks(true));
    CHECK(app_update_status().automatic_checks);
}

TEST_CASE("The Settings note is built only from declared facts", "[updates]") {
    auto s = spectr_like();
    CHECK(app_update_note(s) ==
          "Updates download from Spectr's GitHub releases. Installing an update quits and "
          "reopens Spectr, briefly stopping its audio, and asks for an administrator "
          "password. Updates are never installed automatically.");

    // An app bundle update asks for no password; the note must not claim one.
    s.installer = AppUpdateInstaller::app_bundle;
    const auto app_note = app_update_note(s);
    CHECK(app_note.find("administrator") == std::string::npos);
    CHECK(app_note.find("quits and reopens Spectr") != std::string::npos);

    // Undeclared installer: say nothing about installing.
    s.installer = AppUpdateInstaller::unknown;
    CHECK(app_update_note(s).find("Installing") == std::string::npos);

    // Automatic installation allowed: never promise it cannot happen.
    s.automatic_install = true;
    CHECK(app_update_note(s).find("never installed automatically") == std::string::npos);

    // A non-GitHub releases page is named by host; no releases page falls
    // back to the feed host.
    s.releases_url = "https://updates.example.com/app/";
    CHECK(app_update_note(s).rfind("Updates download from updates.example.com.", 0) == 0);
    s.releases_url.clear();
    s.feed_host = "cdn.example.net";
    CHECK(app_update_note(s).rfind("Updates download from cdn.example.net.", 0) == 0);

    // The development stub says plainly that it has no feed.
    auto stub = spectr_like();
    stub.stub = true;
    CHECK(app_update_note(stub).find("no update feed") != std::string::npos);
}

TEST_CASE("Version and last-check text", "[updates]") {
    auto s = spectr_like();
    CHECK(app_update_version_text(s) == "Version 1.0.7");
    s.build = "1.0.7.2";
    CHECK(app_update_version_text(s) == "Version 1.0.7 (1.0.7.2)");
    CHECK(app_update_last_check_text(s) == "Last checked: never");
    s.last_check_unix_seconds = 1'790'000'000;
    const auto text = app_update_last_check_text(s);
    CHECK(text.rfind("Last checked: 20", 0) == 0);
    CHECK(text.size() == std::string("Last checked: YYYY-MM-DD HH:MM").size());
}

TEST_CASE("Bridge messages report unavailable in a plug-in and drive the service in "
          "the standalone",
          "[updates][bridge]") {
    pulp::view::EditorBridge bridge;
    add_app_update_handlers(bridge);
    REQUIRE(bridge.has_handler(kAppUpdatesGetMessage));
    REQUIRE(bridge.has_handler(kAppUpdatesCheckMessage));
    REQUIRE(bridge.has_handler(kAppUpdatesSetAutomaticMessage));
    REQUIRE(bridge.has_handler(kAppUpdatesOpenReleasesMessage));

    SECTION("plug-in: ok, but not available, and a check starts nothing") {
        set_app_update_service(nullptr);
        auto got = dispatch(bridge, R"({"type":"pulp_updates_get","payload":{}})");
        CHECK(got["ok"].getBool());
        CHECK_FALSE(got["available"].getBool());
        auto checked = dispatch(bridge, R"({"type":"pulp_updates_check","payload":{}})");
        CHECK(checked["ok"].getBool());
        CHECK_FALSE(checked["started"].getBool());
        auto opened = dispatch(bridge, R"({"type":"pulp_updates_open_releases","payload":{}})");
        CHECK_FALSE(opened["opened"].getBool());
    }

    SECTION("standalone: status, check, automatic toggle") {
        ScopedService scope(spectr_like());
        auto got = dispatch(bridge, R"({"type":"pulp_updates_get","payload":{}})");
        CHECK(got["available"].getBool());
        CHECK(got["appName"].getString() == "Spectr");
        CHECK(got["installer"].getString() == "package");
        CHECK(got["releasesUrl"].getString() == "https://github.com/danielraffel/spectr/releases");
        CHECK(got["lastCheckText"].getString() == "Last checked: never");
        CHECK(std::string(got["note"].getString()).find("GitHub releases") != std::string::npos);

        auto checked = dispatch(bridge, R"({"type":"pulp_updates_check","payload":{}})");
        CHECK(checked["started"].getBool());
        CHECK(scope.service->checks == 1);
        CHECK(checked["lastCheckUnixSeconds"].getWithDefault<double>(0) > 0);

        auto on =
            dispatch(bridge, R"({"type":"pulp_updates_set_automatic","payload":{"on":true}})");
        CHECK(on["applied"].getBool());
        CHECK(on["automaticChecks"].getBool());
        CHECK(scope.service->automatic_writes == 1);

        // A malformed request changes nothing.
        auto bad =
            dispatch(bridge, R"({"type":"pulp_updates_set_automatic","payload":{"on":"yes"}})");
        CHECK_FALSE(bad["ok"].getBool());
        CHECK(scope.service->automatic_writes == 1);

        auto opened = dispatch(bridge, R"({"type":"pulp_updates_open_releases","payload":{}})");
        CHECK(opened["opened"].getBool());
        CHECK(scope.service->releases_opened == 1);
    }
}

TEST_CASE("The native Settings group mirrors and drives the service", "[updates][view]") {
    ScopedService scope(spectr_like());
    AppUpdatesSettingsView view;
    CHECK(view.visible());
    REQUIRE(view.automatic_checks_toggle() != nullptr);
    REQUIRE(view.check_button() != nullptr);
    CHECK_FALSE(view.automatic_checks_toggle()->is_on());
    CHECK(view.check_button()->is_enabled());
    CHECK(view.status_label()->text() == "Version 1.0.7 \xC2\xB7 Last checked: never");
    CHECK(view.note_label()->text() == app_update_note(scope.service->next));

    // The button is what a click fires; it runs a check and re-reads status.
    REQUIRE(static_cast<bool>(view.check_button()->on_click));
    view.check_button()->on_click();
    CHECK(scope.service->checks == 1);
    CHECK(view.status_label()->text().find("never") == std::string::npos);

    // The toggle writes the preference through the service.
    REQUIRE(static_cast<bool>(view.automatic_checks_toggle()->on_toggle));
    view.automatic_checks_toggle()->on_toggle(true);
    CHECK(scope.service->next.automatic_checks);

    // A preference changed elsewhere (Sparkle's own prompt) shows on refresh.
    scope.service->next.automatic_checks = false;
    view.refresh();
    CHECK_FALSE(view.automatic_checks_toggle()->is_on());

    // A check already running disables the button.
    scope.service->next.can_check_now = false;
    view.refresh();
    CHECK_FALSE(view.check_button()->is_enabled());

    // The releases button opens the declared page, and hides without one.
    REQUIRE(view.releases_button() != nullptr);
    CHECK(view.releases_button()->visible());
    view.releases_button()->on_click();
    CHECK(scope.service->releases_opened == 1);
    scope.service->next.releases_url.clear();
    view.refresh();
    CHECK_FALSE(view.releases_button()->visible());

    // With the service gone the group hides itself.
    set_app_update_service(nullptr);
    view.refresh();
    CHECK_FALSE(view.visible());
}

namespace {

bool panel_has_tab(pulp::format::SettingsPanel& panel, std::string_view title) {
    return panel.set_active_tab(title);
}

} // namespace

TEST_CASE("The standalone Settings panel offers an Updates tab only when the app can "
          "update itself",
          "[updates][standalone]") {
    pulp::format::StandaloneConfig config;
    config.show_settings_tab = true;

    SECTION("no service: no Updates tab (negative control)") {
        set_app_update_service(nullptr);
        auto chrome = detail::make_standalone_editor_chrome(std::make_unique<pulp::view::View>(),
                                                            config, nullptr, nullptr, nullptr, {});
        REQUIRE(chrome.settings_panel() != nullptr);
        CHECK(panel_has_tab(*chrome.settings_panel(), "MIDI")); // the instrument works
        CHECK_FALSE(panel_has_tab(*chrome.settings_panel(), "Updates"));
    }

    SECTION("service installed: the Updates tab is there") {
        ScopedService scope(spectr_like());
        auto chrome = detail::make_standalone_editor_chrome(std::make_unique<pulp::view::View>(),
                                                            config, nullptr, nullptr, nullptr, {});
        REQUIRE(chrome.settings_panel() != nullptr);
        CHECK(panel_has_tab(*chrome.settings_panel(), "Updates"));
    }
}
