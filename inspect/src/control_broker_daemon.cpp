#include "control_broker_daemon.hpp"

#include "control_private_store.hpp"
#include "control_static_code_identity.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/inter_process_lock.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <sys/acl.h>
#endif
#endif
#endif

namespace pulp::inspect {
namespace {

using namespace std::chrono_literals;

std::uint64_t random_process_generation() {
    const auto bytes = runtime::secure_random_bytes(sizeof(std::uint64_t));
    if (!bytes)
        return 0;
    std::uint64_t value = 0;
    std::memcpy(&value, bytes->data(), sizeof(value));
    return value == 0 ? 1 : value;
}

bool endpoint_accepts_connections(const std::filesystem::path& endpoint) {
    events::InterprocessConnection probe;
    if (!probe.connect(endpoint.string(), events::IpcTransport::LocalSocket, 250ms))
        return false;
    probe.disconnect();
    return true;
}

std::filesystem::path current_user_home_directory() {
#ifdef _WIN32
    return {};
#else
    const auto buffer_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(buffer_size > 0 ? static_cast<std::size_t>(buffer_size)
                                             : std::size_t{16 * 1024});
    passwd entry{};
    passwd* result = nullptr;
    if (::getpwuid_r(::geteuid(), &entry, buffer.data(), buffer.size(), &result) != 0 ||
        result == nullptr || entry.pw_dir == nullptr || entry.pw_dir[0] == '\0') {
        return {};
    }
    return std::filesystem::path{entry.pw_dir}.lexically_normal();
#endif
}

std::filesystem::path default_control_state_directory() {
    const auto home = current_user_home_directory();
    if (home.empty())
        return {};
    return (home / ".pulp" / "state" / "control-broker" / "v1").lexically_normal();
}

#ifndef _WIN32
bool has_no_extended_allow_acl(const std::filesystem::path& directory) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    errno = 0;
    acl_t acl = ::acl_get_file(directory.c_str(), ACL_TYPE_EXTENDED);
    const int error = errno;
    if (acl == nullptr)
        return error == ENOENT || error == ENOATTR;

    acl_entry_t entry{};
    int entry_id = ACL_FIRST_ENTRY;
    while (::acl_get_entry(acl, entry_id, &entry) == 0) {
        acl_tag_t tag{};
        if (::acl_get_tag_type(entry, &tag) != 0 || tag == ACL_EXTENDED_ALLOW) {
            ::acl_free(acl);
            return false;
        }
        entry_id = ACL_NEXT_ENTRY;
    }
    ::acl_free(acl);
    return true;
#else
    (void)directory;
    return true;
#endif
}

bool safe_directory(const std::filesystem::path& directory, bool require_private) {
    struct stat status{};
    if (::lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        return false;
    }
    return has_no_extended_allow_acl(directory) &&
           (!require_private || (status.st_uid == ::geteuid() && (status.st_mode & 07777) == 0700));
}

bool state_path_ancestors_are_safe(std::filesystem::path cursor, bool leaf_is_state_root) {
    bool require_private = leaf_is_state_root;
    while (!cursor.empty()) {
        struct stat status{};
        if (!safe_directory(cursor, require_private) || ::lstat(cursor.c_str(), &status) != 0)
            return false;
        if (!require_private && (status.st_mode & 0022) != 0) {
            return (status.st_mode & S_ISVTX) != 0;
        }
        if (cursor == cursor.root_path())
            return true;
        require_private = false;
        cursor = cursor.parent_path();
    }
    return false;
}
#else
bool state_path_ancestors_are_safe(const std::filesystem::path&, bool) {
    return false;
}
#endif

bool prepare_control_state_directory(const std::filesystem::path& state_directory) {
    if (!state_directory.is_absolute() || state_directory.filename().empty() ||
        state_directory.lexically_normal() != state_directory) {
        return false;
    }
#ifdef _WIN32
    return false;
#else
    std::vector<std::filesystem::path> missing;
    auto cursor = state_directory;
    struct stat status{};
    while (::lstat(cursor.c_str(), &status) != 0) {
        if (errno != ENOENT || cursor == cursor.root_path() || cursor.empty())
            return false;
        missing.push_back(cursor);
        cursor = cursor.parent_path();
    }
    if (!state_path_ancestors_are_safe(cursor, false))
        return false;

    for (auto iterator = missing.rbegin(); iterator != missing.rend(); ++iterator) {
        if (::mkdir(iterator->c_str(), 0700) != 0 || !safe_directory(*iterator, true)) {
            return false;
        }
    }
    return state_path_ancestors_are_safe(state_directory, true) &&
           detail::ensure_owner_private_directory(state_directory);
#endif
}

bool path_is_at_or_below(const std::filesystem::path& candidate,
                         const std::filesystem::path& ancestor) {
    auto candidate_part = candidate.begin();
    for (auto ancestor_part = ancestor.begin(); ancestor_part != ancestor.end();
         ++ancestor_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *candidate_part != *ancestor_part)
            return false;
    }
    return true;
}

bool roots_overlap(const std::filesystem::path& first, const std::filesystem::path& second) {
    return path_is_at_or_below(first, second) || path_is_at_or_below(second, first);
}

} // namespace

struct ControlBrokerDaemon::Impl {
    explicit Impl(ControlBrokerDaemonConfig input) : config(std::move(input)) {}

    ControlBrokerDaemonConfig config;
    std::filesystem::path runtime_directory;
    std::filesystem::path state_directory;
    std::filesystem::path endpoint;
    std::unique_ptr<runtime::InterProcessLock> singleton;
    std::unique_ptr<ControlBroker> broker;
    std::unique_ptr<ControlService> service;
    std::unique_ptr<ControlEndpoint> carrier;

    void reset() noexcept {
        if (carrier)
            carrier->stop();
        carrier.reset();
        service.reset();
        broker.reset();
        if (singleton)
            singleton->unlock();
        singleton.reset();
    }

    bool start() {
        if (carrier && carrier->is_listening())
            return true;
        if (config.sdk_version.empty())
            return false;

        runtime_directory = config.runtime_root.empty()
                                ? default_control_runtime_directory()
                                : default_control_runtime_directory(config.runtime_root);
        state_directory =
            config.state_root.empty() ? default_control_state_directory() : config.state_root;
        state_directory = state_directory.lexically_normal();
        if (runtime_directory.empty() || state_directory.empty() ||
            roots_overlap(runtime_directory, state_directory)) {
            reset();
            return false;
        }
        endpoint = runtime_directory / "broker.sock";
        const auto singleton_name =
            "pulp-control-broker-v1-" + runtime::sha256_hex(endpoint.string());
        singleton = std::make_unique<runtime::InterProcessLock>(singleton_name);
        if (!singleton->try_lock()) {
            reset();
            return false;
        }

        if (!prepare_control_runtime_directory(runtime_directory)) {
            reset();
            return false;
        }
        if (!state_path_ancestors_are_safe(runtime_directory, true)) {
            reset();
            return false;
        }
        if (!prepare_control_state_directory(state_directory)) {
            reset();
            return false;
        }
        if (const auto stale = control_endpoint_identity(endpoint); stale) {
            if (endpoint_accepts_connections(endpoint) ||
                !remove_stale_control_endpoint(endpoint, *stale)) {
                reset();
                return false;
            }
        }

        ControlBrokerConfig broker_config;
        broker_config.operation_store = ControlOperationStoreConfig{
            .directory = state_directory / "operations",
        };
        broker_config.artifact_store = ControlArtifactStoreConfig{
            .root = state_directory / "artifacts",
        };
        broker = std::make_unique<ControlBroker>(std::move(broker_config));
        if (!broker->operation_store_ready() || !broker->artifact_store_ready()) {
            reset();
            return false;
        }
        service = std::make_unique<ControlService>(*broker);

        std::optional<ControlTrustedHostStaticExpectation> daemon_identity;
        if (!config.executable_path.empty())
            daemon_identity = detail::inspect_static_code_identity(config.executable_path);
        if (!config.executable_path.empty() && !daemon_identity) {
            reset();
            return false;
        }
        std::vector<ControlTrustedHostStaticExpectation> trusted_clients;
        if (daemon_identity) {
            const auto broker_directory = config.executable_path.parent_path();
            for (const auto& candidate :
                 {broker_directory / "pulp", broker_directory / "pulp-cpp",
                  broker_directory.parent_path() / "pulp",
                  broker_directory.parent_path() / "tools" / "cli" / "pulp-cpp",
                  broker_directory.parent_path().parent_path() / "bin" / "pulp",
                  broker_directory.parent_path().parent_path() / "bin" / "pulp-cpp"}) {
                if (const auto identity = detail::inspect_static_code_identity(candidate))
                    trusted_clients.push_back(*identity);
            }
        }

        const auto generation = config.process_generation != 0 ? config.process_generation
                                                               : random_process_generation();
        if (generation == 0) {
            reset();
            return false;
        }
        carrier = std::make_unique<ControlEndpoint>(
            *service,
            [](std::string_view) -> std::optional<ControlConnectionAdmission> {
                return std::nullopt;
            },
            ControlEndpointConfig{
                .endpoint_path = endpoint,
                .sdk_version = config.sdk_version,
                .broker_id = broker->broker_id().value,
                .process_generation = generation,
                .authorize_client =
                    daemon_identity
                        ? std::function<bool(
                              const ControlPeerEvidence&)>{[trusted_clients =
                                                                std::move(trusted_clients)](
                                                               const ControlPeerEvidence& peer) {
                              return peer.role == ControlPeerRole::Client &&
                                     std::ranges::any_of(
                                         trusted_clients, [&](const auto& expected) {
                                             return peer.executable_identity ==
                                                        expected.executable_identity &&
                                                    peer.publisher_id == expected.publisher_id;
                                         });
                          }}
                        : std::function<bool(const ControlPeerEvidence&)>{},
            },
            nullptr, nullptr, broker.get());
        if (!carrier->start()) {
            reset();
            return false;
        }
        return true;
    }
};

ControlBrokerDaemon::ControlBrokerDaemon(ControlBrokerDaemonConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ControlBrokerDaemon::~ControlBrokerDaemon() {
    stop();
}

bool ControlBrokerDaemon::start() {
    return impl_->start();
}

void ControlBrokerDaemon::stop() noexcept {
    impl_->reset();
}

bool ControlBrokerDaemon::is_running() const noexcept {
    return impl_->carrier && impl_->carrier->is_listening();
}

const std::filesystem::path& ControlBrokerDaemon::endpoint_path() const noexcept {
    return impl_->endpoint;
}

const std::filesystem::path& ControlBrokerDaemon::state_directory() const noexcept {
    return impl_->state_directory;
}

} // namespace pulp::inspect
