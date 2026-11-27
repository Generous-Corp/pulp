// The Vellum Updates settings group, the port of
// pulp::format::AppUpdatesSettingsView. The cases are the legacy view's,
// against the same fake AppUpdateService.

#include <pulp/vellum/app_updates_settings_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/app_updates.hpp>

#include <vellum/view/buttons.hpp>
#include <vellum/view/widgets.hpp>

#include <memory>
#include <string>

using namespace pulp::format;
using pulp::vellum::AppUpdatesSettingsView;

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

} // namespace

TEST_CASE("The Vellum Settings group mirrors and drives the service",
          "[pulp-vellum][updates][view]") {
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

    // A preference changed elsewhere shows on refresh (and on the idle tick).
    scope.service->next.automatic_checks = false;
    view.editor_idle();
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
