#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_host_connection.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {
const volatile char kStandalone[] = "PULP_STANDALONE_COMPONENT_V1";
const volatile char kShipping[] = "PULP_INSPECT_SHIPPING_MANIFEST_V1";
const volatile char kProfile[] = "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1";
const volatile char kManifest[] =
    "PULP_CONTROL_MANIFEST_SHA256_971c6799c570d0aeb8afaa6acdc10890414703a77138706b390a81496295a2dd_"
    "V1";
const volatile char kCapability[] = "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1";

bool send_and_drop(std::string_view endpoint, std::string_view enrollment_id) {
    InterprocessConnection connection;
    connection.set_max_message_bytes(kControlMaximumEnvelopeBytes);
    connection.set_write_timeout(3s);
    if (!connection.connect(std::string(endpoint), IpcTransport::LocalSocket, 3s))
        return false;
    const auto encoded = encode_control_envelope(ControlEnvelope{
        .payload = ControlHostOpenEnvelope{.request_id = "fixture-drop",
                                           .enrollment_id = std::string(enrollment_id)}});
    const bool sent = !encoded.empty() && connection.send_message(encoded);
    std::this_thread::sleep_for(10ms);
    connection.disconnect();
    return sent;
}

bool publish_result(const std::filesystem::path& path, unsigned accepted, unsigned denied,
                    std::string_view registration_id = {}) {
    auto staging = path;
    staging += ".tmp";
    {
        std::ofstream output(staging, std::ios::trunc);
        if (!output)
            return false;
        output << accepted << ' ' << denied << ' ' << registration_id << '\n';
        output.close();
        if (!output)
            return false;
    }
    std::error_code error;
    std::filesystem::rename(staging, path, error);
    return !error;
}
} // namespace

int main() {
    if (kStandalone[0] != 'P' || kShipping[0] != 'P' || kProfile[0] != 'P' || kManifest[0] != 'P' ||
        kCapability[0] != 'P')
        return 9;
    std::string preflight_endpoint;
    if (!std::getline(std::cin, preflight_endpoint))
        return 8;
    InterprocessConnection preflight;
    if (!preflight.connect(preflight_endpoint, IpcTransport::LocalSocket, 3s))
        return 7;
    const auto broker_evidence =
        observe_control_peer(preflight, ControlPeerRole::TrustedHostBridge);
    if (!broker_evidence)
        return 6;

    std::string endpoint;
    std::string enrollment_id;
    std::string result_path;
    std::string stop_path;
    std::string mode;
    if (!std::getline(std::cin, endpoint) || !std::getline(std::cin, enrollment_id) ||
        !std::getline(std::cin, result_path) || !std::getline(std::cin, stop_path) ||
        !std::getline(std::cin, mode))
        return 8;
    preflight.disconnect();

    std::atomic<unsigned> accepted{0};
    std::atomic<unsigned> denied{0};
    std::mutex registration_mutex;
    std::string registration_id;
    if (mode == "drop") {
        const bool sent = send_and_drop(endpoint, enrollment_id);
        if (!publish_result(result_path, 0, 0))
            return 4;
        return sent ? 0 : 3;
    }
    const auto make_connection = [&] {
        return std::make_unique<ControlHostConnection>(
            ControlHostConnectionConfig{.endpoint_path = endpoint,
                                        .expected_broker = {.evidence = *broker_evidence}},
            [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
               const ControlExecutionContext&) {
                return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
            });
    };
    auto first = make_connection();
    auto second = make_connection();
    auto run = [&](ControlHostConnection& connection) {
        if (!connection.connect())
            return;
        const auto result = connection.open_host_enrollment(enrollment_id);
        if (result.accepted) {
            accepted.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(registration_mutex);
            registration_id = result.registration_id;
        } else {
            denied.fetch_add(1, std::memory_order_relaxed);
        }
    };
    if (mode == "concurrent") {
        std::thread one([&] { run(*first); });
        std::thread two([&] { run(*second); });
        one.join();
        two.join();
    } else {
        run(*first);
        if (mode == "replay" && accepted.load(std::memory_order_relaxed) == 1) {
            if (!first->open_host_enrollment(enrollment_id).accepted)
                denied.fetch_add(1, std::memory_order_relaxed);
            if (!first->open_host("unused-cross-mode-admission").accepted)
                denied.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!publish_result(result_path, accepted.load(std::memory_order_relaxed),
                        denied.load(std::memory_order_relaxed), registration_id))
        return 4;
    if (accepted.load(std::memory_order_relaxed) != 0 && !stop_path.empty()) {
        for (unsigned attempt = 0; attempt < 5000 && !std::filesystem::exists(stop_path); ++attempt)
            std::this_thread::sleep_for(1ms);
    }
    first->disconnect();
    second->disconnect();
    return accepted.load(std::memory_order_relaxed) != 0 ? 0 : 2;
}
