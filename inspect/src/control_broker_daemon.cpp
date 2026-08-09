#include "control_broker_daemon.hpp"

#include "control_private_store.hpp"
#include "control_static_code_identity.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_host_router.hpp>
#include <pulp/inspect/control_peer.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/inspect/control_trusted_host_launcher.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/inter_process_lock.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
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

bool stale_observer_name(std::string_view name) {
    constexpr std::string_view prefix = "bo-";
    constexpr std::string_view suffix = ".sock";
    if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
        name.size() != prefix.size() + 16 + suffix.size()) {
        return false;
    }
    const auto nonce = name.substr(prefix.size(), 16);
    return std::ranges::all_of(nonce, [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

bool remove_stale_observer_sockets(const std::filesystem::path& runtime_directory) {
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(runtime_directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (!stale_observer_name(entry.path().filename().string()))
            continue;
        const auto status = entry.symlink_status(error);
        if (error)
            return false;
        if (status.type() == std::filesystem::file_type::socket)
            (void)std::filesystem::remove(entry.path(), error);
        if (error)
            return false;
    }
    return !error;
}

std::optional<ControlPeerEvidence>
observe_current_broker(const std::filesystem::path& observer_endpoint,
                       const ControlTrustedHostStaticExpectation& static_identity) {
    events::InterprocessConnectionServer observer;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<events::InterprocessConnection> accepted;
    observer.on_client_connected = [&](auto connection) {
        {
            std::lock_guard lock(mutex);
            accepted = std::move(connection);
        }
        ready.notify_all();
    };
    if (!observer.start(observer_endpoint.string(), events::IpcTransport::LocalSocket))
        return std::nullopt;
    events::InterprocessConnection client;
    if (!client.connect(observer_endpoint.string(), events::IpcTransport::LocalSocket, 2s)) {
        observer.stop();
        return std::nullopt;
    }
    {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 2s, [&] { return accepted != nullptr; })) {
            client.disconnect();
            observer.stop();
            return std::nullopt;
        }
    }
    auto evidence = observe_control_peer(*accepted, ControlPeerRole::TrustedHostBridge);
    if (evidence && (evidence->process_id != ::getpid() ||
                     evidence->executable_identity != static_identity.executable_identity ||
                     evidence->publisher_id != static_identity.publisher_id))
        evidence.reset();
    client.disconnect();
    accepted.reset();
    observer.stop();
    return evidence;
}

std::filesystem::path current_process_executable() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(buffer.data(), error);
    return error ? std::filesystem::path{} : path;
#else
    return {};
#endif
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

std::optional<ControlTrustedHostPreparationPolicy>
pin_trusted_host_policy(const ControlTrustedHostLaunchIntent& intent) {
#ifdef _WIN32
    (void)intent;
    return std::nullopt;
#else
    constexpr std::size_t maximum_executable_bytes = 512u * 1024u * 1024u;
    constexpr std::size_t maximum_manifest_bytes = 1024u * 1024u;
    const auto executable =
        detail::read_owner_private_file(intent.executable, maximum_executable_bytes);
    const auto manifest_path = std::filesystem::path{
        intent.executable.string() + ".inspector-capabilities.json"};
    const auto manifest = detail::read_owner_private_file(manifest_path, maximum_manifest_bytes);
    const auto static_expectation = detail::inspect_static_code_identity(intent.executable);
    struct stat working_directory{};
    if (!executable || !manifest || !static_expectation ||
        ::lstat(intent.working_directory.c_str(), &working_directory) != 0 ||
        !S_ISDIR(working_directory.st_mode) || S_ISLNK(working_directory.st_mode)) {
        return std::nullopt;
    }
    return ControlTrustedHostPreparationPolicy{
        .executable_digest =
            runtime::sha256_hex(executable->data(), executable->size()),
        .manifest_digest = runtime::sha256_hex(manifest->data(), manifest->size()),
        .static_expectation = *static_expectation,
        .working_directory_device = static_cast<std::uint64_t>(working_directory.st_dev),
        .working_directory_inode = static_cast<std::uint64_t>(working_directory.st_ino),
    };
#endif
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
    std::unique_ptr<ControlHostEnrollmentStore> enrollments;
    std::unique_ptr<ControlConnectionAdmissionStore> admissions;
    std::unique_ptr<ControlHostRouter> host_router;
    std::unique_ptr<ControlEndpointEnrollmentContext> enrollment_context;
    std::unique_ptr<ControlTrustedHostInventory> trusted_inventory;
    std::unique_ptr<ControlTrustedHostLauncher> trusted_launcher;
    struct TrustedHostPolicyEntry {
        ControlTrustedHostLaunchIntent intent;
        ControlTrustedHostPreparationPolicy policy;
    };
    std::vector<TrustedHostPolicyEntry> trusted_host_policies;
    struct InstalledHostPolicyEntry {
        std::string host_id;
        ControlTrustedHostLaunchIntent intent;
        ControlTrustedHostPreparationPolicy policy;
    };
    std::vector<InstalledHostPolicyEntry> installed_host_policies;
    std::mutex launched_hosts_mutex;
    std::vector<std::unique_ptr<platform::ChildProcess>> launched_hosts;
    std::unique_ptr<ControlService> service;
    std::unique_ptr<ControlEndpoint> carrier;

    void reset() noexcept {
        if (carrier)
            carrier->stop();
        carrier.reset();
        {
            std::lock_guard lock(launched_hosts_mutex);
            launched_hosts.clear();
        }
        service.reset();
        trusted_launcher.reset();
        enrollment_context.reset();
        if (host_router)
            host_router->stop();
        host_router.reset();
        admissions.reset();
        enrollments.reset();
        trusted_inventory.reset();
        trusted_host_policies.clear();
        installed_host_policies.clear();
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
        if (!remove_stale_observer_sockets(runtime_directory)) {
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

        host_router = std::make_unique<ControlHostRouter>();
        auto* router = host_router.get();
        ControlBrokerConfig broker_config;
        broker_config.admission.host_available =
            [router](const ControlRegistration& registration, const auto&) {
                return router->connected(registration.registration_id);
            };
        broker_config.admission.activated =
            [router](const ControlRegistration& registration, const auto&) {
                return router->connected(registration.registration_id);
            };
        broker_config.admission.policy_eligible = [](const auto&, const auto& operation) {
            return capability_is_grantable(operation.capability);
        };
        broker_config.operation_store = ControlOperationStoreConfig{
            .directory = state_directory / "operations",
        };
        broker_config.artifact_store = ControlArtifactStoreConfig{
            .root = state_directory / "artifacts",
        };
        broker_config.process_liveness = control_peer_process_liveness;
        broker = std::make_unique<ControlBroker>(std::move(broker_config));
        if (!broker->operation_store_ready() || !broker->artifact_store_ready()) {
            reset();
            return false;
        }
        trusted_host_policies.clear();
        trusted_host_policies.reserve(config.trusted_host_allowlist.size());
        for (const auto& intent : config.trusted_host_allowlist) {
            const auto policy = pin_trusted_host_policy(intent);
            if (!policy) {
                reset();
                return false;
            }
            trusted_host_policies.push_back({intent, *policy});
        }
        installed_host_policies.clear();
        installed_host_policies.reserve(config.installed_host_selections.size());
        for (const auto& selection : config.installed_host_selections) {
            const bool valid_id = !selection.host_id.empty() && selection.host_id.size() <= 128 &&
                                  std::ranges::all_of(selection.host_id, [](unsigned char value) {
                                      return (value >= 'a' && value <= 'z') ||
                                             (value >= 'A' && value <= 'Z') ||
                                             (value >= '0' && value <= '9') || value == '-' ||
                                             value == '_';
                                  });
            const bool duplicate = std::ranges::any_of(
                installed_host_policies,
                [&](const auto& entry) { return entry.host_id == selection.host_id; });
            const auto policy = pin_trusted_host_policy(selection.intent);
            if (!valid_id || duplicate || !policy) {
                reset();
                return false;
            }
            installed_host_policies.push_back(
                {selection.host_id, selection.intent, *policy});
        }
        const auto actual_executable = current_process_executable();
        const auto broker_static_identity =
            detail::inspect_static_code_identity(actual_executable);
        std::error_code executable_error;
        const auto configured_executable = config.executable_path.empty()
                                               ? std::filesystem::path{}
                                               : std::filesystem::weakly_canonical(
                                                     config.executable_path, executable_error);
        if (actual_executable.empty() || !broker_static_identity || executable_error ||
            (!configured_executable.empty() && configured_executable != actual_executable)) {
            reset();
            return false;
        }
        const auto daemon_identity = config.executable_path.empty()
                                         ? std::optional<ControlTrustedHostStaticExpectation>{}
                                         : broker_static_identity;
        std::vector<ControlTrustedHostStaticExpectation> trusted_clients;
        std::vector<ControlTrustedHostStaticExpectation> trusted_mcp_clients;
        if (daemon_identity) {
            const auto broker_directory = config.executable_path.parent_path();
            for (const auto& candidate :
                 {broker_directory / "pulp", broker_directory / "pulp-cpp",
                  broker_directory / "pulp-mcp",
                  broker_directory.parent_path() / "pulp",
                  broker_directory.parent_path() / "tools" / "cli" / "pulp-cpp",
                  broker_directory.parent_path() / "tools" / "mcp" / "pulp-mcp",
                  broker_directory.parent_path().parent_path() / "bin" / "pulp",
                  broker_directory.parent_path().parent_path() / "bin" / "pulp-cpp",
                  broker_directory.parent_path().parent_path() / "bin" / "pulp-mcp"}) {
                if (const auto identity = detail::inspect_static_code_identity(candidate)) {
                    trusted_clients.push_back(*identity);
                    if (candidate.filename() == "pulp-mcp")
                        trusted_mcp_clients.push_back(*identity);
                }
            }
        }

        const auto generation = config.process_generation != 0 ? config.process_generation
                                                               : random_process_generation();
        if (generation == 0) {
            reset();
            return false;
        }
        const auto observer_nonce = runtime::secure_random_bytes(8);
        if (!observer_nonce) {
            reset();
            return false;
        }
        const auto broker_peer = observe_current_broker(
            runtime_directory /
                ("bo-" + runtime::hex_encode(*observer_nonce) + ".sock"),
            *broker_static_identity);
        if (!broker_peer) {
            reset();
            return false;
        }

        enrollments = std::make_unique<ControlHostEnrollmentStore>();
        admissions = std::make_unique<ControlConnectionAdmissionStore>();
        enrollment_context = std::make_unique<ControlEndpointEnrollmentContext>(
            ControlEndpointEnrollmentContext{*enrollments, *broker, *admissions});
        trusted_inventory = std::make_unique<ControlTrustedHostInventory>(
            ControlTrustedHostInventoryConfig{
                .staging_root = runtime_directory / "trusted-hosts",
                .broker_generation = generation,
            });
        trusted_launcher = std::make_unique<ControlTrustedHostLauncher>(
            *trusted_inventory, *enrollments,
            ControlTrustedHostLauncherConfig{
                .endpoint_path = endpoint,
                .expected_broker = {.evidence = *broker_peer},
                .broker_generation = generation,
                // Fresh signed hosts can spend several seconds in dyld and
                // code-signature validation on a busy macOS machine.
                .preflight_timeout = 10s,
            });
        service = std::make_unique<ControlService>(*broker, host_router->executor());
        auto* admission_store = admissions.get();
        carrier = std::make_unique<ControlEndpoint>(
            *service, [admission_store](std::string_view id) {
                return admission_store->consume(id);
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
                .durable_client_principal =
                    daemon_identity
                        ? std::function<std::optional<ControlEndpointConfig::DurableClientPrincipal>(
                              const ControlPeerEvidence&)>{[trusted_mcp_clients](
                                                               const ControlPeerEvidence& peer) {
                              const bool process_scoped = std::ranges::any_of(
                                  trusted_mcp_clients, [&](const auto& expected) {
                                      return peer.executable_identity ==
                                                 expected.executable_identity &&
                                             peer.publisher_id == expected.publisher_id;
                                  });
                              std::string canonical;
                              canonical.reserve(peer.user_id.size() +
                                                peer.executable_identity.size() +
                                                peer.publisher_id.size() +
                                                peer.process_start_id.size() + 32);
                              canonical.append(peer.user_id);
                              canonical.push_back('\0');
                              canonical.append(peer.executable_identity);
                              canonical.push_back('\0');
                              canonical.append(peer.publisher_id);
                              if (process_scoped) {
                                  // An MCP server is a long-lived client process. Reconnects
                                  // opened by that process must retain its grants/receipts,
                                  // while a separate server using the same signed executable
                                  // must not supersede it. PID plus the kernel process
                                  // generation supplies that distinction without accepting
                                  // any caller-provided identity material.
                                  canonical.push_back('\0');
                                  canonical.append(std::to_string(peer.process_id));
                                  canonical.push_back('\0');
                                  canonical.append(peer.process_start_id);
                              }
                              return std::optional{
                                  ControlEndpointConfig::DurableClientPrincipal{
                                      .value =
                                          std::string(process_scoped ? "installed-mcp-"
                                                                     : "installed-cli-") +
                                          runtime::hex_encode(runtime::sha256(canonical)),
                                      .lifetime =
                                          process_scoped
                                              ? ControlDurableClientLifetime::Process
                                              : ControlDurableClientLifetime::Broker,
                                  }};
                          }}
                        : std::function<std::optional<
                              ControlEndpointConfig::DurableClientPrincipal>(
                              const ControlPeerEvidence&)>{},
                .decide_consent = config.decide_consent,
                .trusted_hosts =
                    {
                        .prepare = trusted_host_policies.empty()
                                       ? std::function<ControlTrustedHostInventoryPrepareResult(
                                             const ControlTrustedHostLaunchIntent&)>{}
                                       : std::function<ControlTrustedHostInventoryPrepareResult(
                                             const ControlTrustedHostLaunchIntent&)>{
                                             [this](const auto& intent) {
                                                 const auto allowed = std::ranges::find_if(
                                                     trusted_host_policies,
                                                     [&](const auto& entry) {
                                                         return entry.intent == intent;
                                                     });
                                                 if (!trusted_inventory ||
                                                     allowed == trusted_host_policies.end())
                                                     return ControlTrustedHostInventoryPrepareResult{};
                                                 return trusted_inventory->prepare(intent,
                                                                                   allowed->policy);
                                             }},
                        .prepare_installed = [this](std::string_view host_id) {
                            const auto selected = std::ranges::find_if(
                                installed_host_policies,
                                [&](const auto& entry) { return entry.host_id == host_id; });
                            if (!trusted_inventory || selected == installed_host_policies.end())
                                return ControlTrustedHostInventoryPrepareResult{};
                            return trusted_inventory->prepare(selected->intent,
                                                              selected->policy);
                        },
                        .launch = [this](std::string_view inventory_id) {
                            if (!trusted_launcher)
                                return ControlTrustedHostManagementLaunchResult{};
                            platform::ProcessOptions options;
                            options.capture_stdout = false;
                            options.capture_stderr = false;
                            auto result = trusted_launcher->launch(inventory_id, std::move(options));
                            ControlTrustedHostManagementLaunchResult management_result{
                                .status = result.status,
                                .explanation = std::move(result.explanation),
                            };
                            if (result.launched()) {
                                std::lock_guard lock(launched_hosts_mutex);
                                std::erase_if(launched_hosts,
                                              [](const auto& process) {
                                                  return !process->is_running();
                                              });
                                launched_hosts.push_back(std::move(result.process));
                            }
                            return management_result;
                        },
                    },
            },
            host_router.get(), enrollment_context.get(), broker.get());
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
