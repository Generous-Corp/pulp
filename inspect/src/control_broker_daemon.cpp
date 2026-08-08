#include "control_broker_daemon.hpp"

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/inter_process_lock.hpp>

#include <chrono>
#include <cstring>
#include <optional>
#include <utility>

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

} // namespace

struct ControlBrokerDaemon::Impl {
    explicit Impl(ControlBrokerDaemonConfig input) : config(std::move(input)) {}

    ControlBrokerDaemonConfig config;
    std::filesystem::path runtime_directory;
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
        if (const auto stale = control_endpoint_identity(endpoint); stale) {
            if (endpoint_accepts_connections(endpoint) ||
                !remove_stale_control_endpoint(endpoint, *stale)) {
                reset();
                return false;
            }
        }

        ControlBrokerConfig broker_config;
        broker_config.operation_store = ControlOperationStoreConfig{
            .directory = runtime_directory / "operations",
        };
        broker_config.artifact_store = ControlArtifactStoreConfig{
            .root = runtime_directory / "artifacts",
        };
        broker = std::make_unique<ControlBroker>(std::move(broker_config));
        if (!broker->operation_store_ready() || !broker->artifact_store_ready()) {
            reset();
            return false;
        }
        service = std::make_unique<ControlService>(*broker);

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
            });
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

} // namespace pulp::inspect
