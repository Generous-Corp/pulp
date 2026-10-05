#include <pulp/format/app_updates.hpp>

#include <choc/text/choc_JSON.h>

#include <ctime>
#include <mutex>
#include <string_view>
#include <utility>

namespace pulp::format {

namespace {

std::mutex& service_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<AppUpdateService>& service_slot() {
    static std::shared_ptr<AppUpdateService> service;
    return service;
}

// "https://github.com/owner/repo/releases" -> "github.com". Empty when the
// string is not an http(s) URL.
std::string url_host(std::string_view url) {
    for (std::string_view scheme : {std::string_view{"https://"}, std::string_view{"http://"}}) {
        if (url.substr(0, scheme.size()) == scheme) {
            url.remove_prefix(scheme.size());
            const auto end = url.find_first_of("/:?#");
            return std::string(url.substr(0, end));
        }
    }
    return {};
}

bool is_github(std::string_view host) {
    return host == "github.com" || host == "www.github.com";
}

const char* installer_name(AppUpdateInstaller installer) {
    switch (installer) {
    case AppUpdateInstaller::package:
        return "package";
    case AppUpdateInstaller::app_bundle:
        return "app";
    case AppUpdateInstaller::unknown:
        break;
    }
    return "unknown";
}

} // namespace

void set_app_update_service(std::shared_ptr<AppUpdateService> service) {
    std::lock_guard lock(service_mutex());
    service_slot() = std::move(service);
}

std::shared_ptr<AppUpdateService> app_update_service() {
    std::lock_guard lock(service_mutex());
    return service_slot();
}

AppUpdateStatus app_update_status() {
    if (auto service = app_update_service()) {
        auto status = service->status();
        status.available = true;
        return status;
    }
    return {};
}

bool check_for_app_updates() {
    auto service = app_update_service();
    return service ? service->check_for_updates() : false;
}

bool set_app_update_automatic_checks(bool on) {
    auto service = app_update_service();
    return service ? service->set_automatic_checks(on) : false;
}

bool open_app_releases_page() {
    auto service = app_update_service();
    return service ? service->open_releases_page() : false;
}

std::string app_update_note(const AppUpdateStatus& status) {
    if (!status.available)
        return {};
    const std::string app = status.app_name.empty() ? std::string("the app") : status.app_name;
    std::string note;
    auto sentence = [&note](std::string text) {
        if (!note.empty())
            note += ' ';
        note += std::move(text);
    };

    if (status.stub) {
        sentence("This development build has no update feed; Check for Updates only "
                 "records the request.");
        return note;
    }

    const std::string releases_host = url_host(status.releases_url);
    if (!releases_host.empty()) {
        sentence(is_github(releases_host) ? "Updates download from " + app + "'s GitHub releases."
                                          : "Updates download from " + releases_host + ".");
    } else if (!status.feed_host.empty()) {
        sentence(is_github(status.feed_host)
                     ? "Updates download from " + app + "'s GitHub releases."
                     : "Updates download from " + status.feed_host + ".");
    }

    switch (status.installer) {
    case AppUpdateInstaller::package:
        sentence("Installing an update quits and reopens " + app +
                 ", briefly stopping its audio, and asks for an administrator password.");
        break;
    case AppUpdateInstaller::app_bundle:
        sentence("Installing an update quits and reopens " + app + ", briefly stopping its audio.");
        break;
    case AppUpdateInstaller::unknown:
        break;
    }

    if (!status.automatic_install)
        sentence("Updates are never installed automatically.");
    return note;
}

std::string app_update_version_text(const AppUpdateStatus& status) {
    if (status.version.empty() && status.build.empty())
        return {};
    if (status.version.empty())
        return "Version " + status.build;
    if (status.build.empty() || status.build == status.version)
        return "Version " + status.version;
    return "Version " + status.version + " (" + status.build + ")";
}

std::string app_update_last_check_text(const AppUpdateStatus& status) {
    if (status.last_check_unix_seconds <= 0)
        return "Last checked: never";
    const std::time_t when = static_cast<std::time_t>(status.last_check_unix_seconds);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &when);
#else
    localtime_r(&when, &local);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return std::string("Last checked: ") + buffer;
}

std::string app_update_status_json(const AppUpdateStatus& status) {
    auto object = choc::value::createObject("PulpAppUpdateStatus");
    object.addMember("available", status.available);
    object.addMember("canCheckNow", status.available && status.can_check_now);
    object.addMember("automaticChecks", status.automatic_checks);
    object.addMember("automaticInstall", status.automatic_install);
    object.addMember("stub", status.stub);
    object.addMember("appName", status.app_name);
    object.addMember("version", status.version);
    object.addMember("build", status.build);
    object.addMember("lastCheckUnixSeconds", static_cast<double>(status.last_check_unix_seconds));
    object.addMember("releasesUrl", status.releases_url);
    object.addMember("feedHost", status.feed_host);
    object.addMember("installer", std::string(installer_name(status.installer)));
    object.addMember("note", app_update_note(status));
    object.addMember("versionText", app_update_version_text(status));
    object.addMember("lastCheckText",
                     status.available ? app_update_last_check_text(status) : std::string());
    return choc::json::toString(object);
}

} // namespace pulp::format
