#include "control_broker_daemon.hpp"
#include "control_private_store.hpp"

#include <pulp/runtime/crypto.hpp>

#include <pulp/inspect/control_consent_authority.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <mach-o/dyld.h>

#ifndef PULP_CONTROL_SDK_VERSION
#define PULP_CONTROL_SDK_VERSION "unknown"
#endif

extern "C" __attribute__((used, visibility("default"))) const volatile char
    pulp_control_broker_version_query_v1[] = "PULP_CONTROL_BROKER_VERSION_QUERY_V1";

namespace {

std::atomic<bool> stopping{false};

void request_stop(int) {
    stopping.store(true, std::memory_order_relaxed);
}

std::filesystem::path executable_path() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(buffer.data(), error);
    return error ? std::filesystem::path{} : path;
}

std::filesystem::path environment_path(const char* name) {
    const auto* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path{value} : std::filesystem::path{};
}

bool owner_private_directory(const std::filesystem::path& path) {
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
           !S_ISLNK(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == 0700;
}

bool valid_host_id(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '-' ||
                      character == '_';
           });
}

bool valid_version_id(std::string_view value) {
    constexpr std::string_view prefix = "version-";
    return value.size() == prefix.size() + 64 && value.starts_with(prefix) &&
           std::ranges::all_of(value.substr(prefix.size()), [](unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

std::optional<std::vector<pulp::inspect::ControlInstalledHostSelection>>
installed_host_selections(const std::filesystem::path& broker) {
    if (broker.empty())
        return std::nullopt;
    std::vector<pulp::inspect::ControlInstalledHostSelection> selections;
    const auto host = broker.parent_path() / "pulp-control-standalone-host";
    const auto manifest = std::filesystem::path{
        host.string() + ".inspector-capabilities.json"};
    std::error_code error;
    if (std::filesystem::is_regular_file(host, error) && !error &&
        std::filesystem::is_regular_file(manifest, error) && !error) {
        selections.push_back(
            {.host_id = "ordinary-standalone",
             .intent = {.executable = host,
                        .arguments = {},
                        .working_directory = host.parent_path(),
                        .host_tier = pulp::inspect::ControlHostTier::Standalone}});
    }

    const auto catalog = broker.parent_path() / "control-hosts";
    if (!std::filesystem::exists(catalog, error))
        return error ? std::nullopt : std::optional{std::move(selections)};
    if (error || !owner_private_directory(catalog))
        return std::nullopt;
    for (const auto& entry : std::filesystem::directory_iterator(catalog, error)) {
        const auto filename = entry.path().filename().string();
        if (!filename.empty() && filename.front() == '.')
            continue;
        if (error || selections.size() >= 64 || !entry.is_directory(error) || error ||
            !owner_private_directory(entry.path()))
            return std::nullopt;
        const auto host_id = filename;
        if (!valid_host_id(host_id) || host_id == "ordinary-standalone")
            return std::nullopt;
        const auto active = entry.path() / "active";
        if (!std::filesystem::exists(active, error)) {
            if (error)
                return std::nullopt;
            continue;
        }
        const auto active_bytes = pulp::inspect::detail::read_owner_private_file(active, 128);
        if (!active_bytes)
            return std::nullopt;
        std::string version(active_bytes->begin(), active_bytes->end());
        if (!version.empty() && version.back() == '\n')
            version.pop_back();
        if (!valid_version_id(version))
            return std::nullopt;
        const auto version_directory = entry.path() / version;
        if (!owner_private_directory(version_directory))
            return std::nullopt;
        const auto executable = version_directory / "host";
        const auto capability_manifest =
            version_directory / "host.inspector-capabilities.json";
        const auto closure_manifest = version_directory / "runtime-closure.sha256";
        if (!std::filesystem::is_regular_file(executable, error) || error ||
            !std::filesystem::is_regular_file(capability_manifest, error) || error ||
            !std::filesystem::is_regular_file(closure_manifest, error) || error)
            return std::nullopt;
        const auto closure_bytes =
            pulp::inspect::detail::read_owner_private_file(closure_manifest, 8192);
        if (!closure_bytes ||
            pulp::runtime::sha256_hex(closure_bytes->data(), closure_bytes->size()) !=
                version.substr(std::string_view{"version-"}.size()))
            return std::nullopt;
        selections.push_back(
            {.host_id = host_id,
             .intent = {.executable = executable,
                        .arguments = {},
                        .working_directory = version_directory,
                        .host_tier = pulp::inspect::ControlHostTier::Standalone}});
    }
    if (error)
        return std::nullopt;
    std::ranges::sort(selections, {}, &pulp::inspect::ControlInstalledHostSelection::host_id);
    return selections;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << PULP_CONTROL_SDK_VERSION << '\n';
        return 0;
    }
    if (argc != 1)
        return 2;

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    auto consent_authority = std::make_shared<pulp::inspect::ControlBrokerConsentAuthority>();
    const auto broker = executable_path();
    while (!stopping.load(std::memory_order_relaxed)) {
        const auto installed_hosts = installed_host_selections(broker);
        if (!installed_hosts)
            return 1;
        pulp::inspect::ControlBrokerDaemon daemon({
            .runtime_root = environment_path("PULP_CONTROL_BROKER_RUNTIME_ROOT"),
            .state_root = environment_path("PULP_CONTROL_BROKER_STATE_ROOT"),
            .sdk_version = PULP_CONTROL_SDK_VERSION,
            .executable_path = broker,
            .installed_host_selections = *installed_hosts,
            .decide_consent = [consent_authority](const auto& request) {
                return consent_authority->decide(request);
            },
        });
        if (!daemon.start())
            return 1;
        while (!stopping.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const auto refreshed = installed_host_selections(broker);
            if (!refreshed) {
                daemon.stop();
                return 1;
            }
            if (*refreshed != *installed_hosts)
                break;
        }
        daemon.stop();
    }
    consent_authority->reset();
    return 0;
}
