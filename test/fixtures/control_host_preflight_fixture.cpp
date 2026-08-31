#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace std::chrono_literals;

namespace {

const volatile char kStandalone[] = "PULP_STANDALONE_COMPONENT_V1";
const volatile char kShipping[] = "PULP_INSPECT_SHIPPING_MANIFEST_V1";
const volatile char kProfile[] = "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1";
const volatile char kManifest[] =
    "PULP_CONTROL_MANIFEST_SHA256_6c1a643db61a546e369e8762ae2683d7d9e4ef593709be9eaf7ffa99970e898b_"
    "V1";
const volatile char kCapability[] = "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1";

bool contains_authority_material(std::string_view value) {
    return value.find("admission-1") != std::string_view::npos ||
           value.find("registration-1") != std::string_view::npos;
}

bool send_wrong_nonce(pulp::inspect::ControlHostBootstrapHandle handle) {
    pulp::events::InterprocessConnection connection;
    connection.set_on_message([&connection](const void*, std::size_t) {
        const auto message = pulp::inspect::encode_control_envelope(
            {.payload = pulp::inspect::ControlHostPreflightResponseEnvelope{std::string(64, 'b')}});
        (void)connection.send_message(message);
    });
    if (!connection.attach_inherited_local_socket(handle))
        return false;
#ifndef _WIN32
    ::close(handle);
#endif
    std::this_thread::sleep_for(500ms);
    return true;
}

bool send_malformed(pulp::inspect::ControlHostBootstrapHandle handle) {
    pulp::events::InterprocessConnection connection;
    connection.set_on_message([&connection](const void*, std::size_t) {
        (void)connection.send_message("not a control envelope");
    });
    if (!connection.attach_inherited_local_socket(handle))
        return false;
#ifndef _WIN32
    ::close(handle);
#endif
    std::this_thread::sleep_for(500ms);
    return true;
}

enum class ReceiptMode { Missing, WrongNonce, ReplayedResponse };

bool finish_without_valid_receipt(pulp::inspect::ControlHostBootstrapHandle handle,
                                  ReceiptMode mode) {
    pulp::events::InterprocessConnection connection;
    std::atomic<unsigned> frames{0};
    std::atomic<bool> completed{false};
    connection.set_on_message([&](const void* data, std::size_t size) {
        const auto decoded = pulp::inspect::decode_control_envelope(
            std::string_view(static_cast<const char*>(data), size));
        if (!decoded)
            return;
        const auto frame = frames.fetch_add(1);
        if (frame == 0) {
            const auto* challenge = std::get_if<pulp::inspect::ControlHostPreflightChallengeEnvelope>(
                &decoded->payload);
            if (challenge) {
                const auto response = pulp::inspect::encode_control_envelope(
                    {.payload = pulp::inspect::ControlHostPreflightResponseEnvelope{
                         challenge->nonce}});
                (void)connection.send_message(response);
            }
            return;
        }
        const auto* bootstrap = std::get_if<pulp::inspect::ControlHostPreflightBootstrapEnvelope>(
            &decoded->payload);
        if (!bootstrap)
            return;
        if (mode == ReceiptMode::WrongNonce) {
            const auto receipt = pulp::inspect::encode_control_envelope(
                {.payload = pulp::inspect::ControlHostPreflightReceiptEnvelope{
                     std::string(64, 'b')}});
            (void)connection.send_message(receipt);
        } else if (mode == ReceiptMode::ReplayedResponse) {
            const auto replay = pulp::inspect::encode_control_envelope(
                {.payload = pulp::inspect::ControlHostPreflightResponseEnvelope{
                     bootstrap->nonce}});
            (void)connection.send_message(replay);
        }
        completed.store(true, std::memory_order_release);
    });
    if (!connection.attach_inherited_local_socket(handle))
        return false;
#ifndef _WIN32
    ::close(handle);
#endif
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    if (mode != ReceiptMode::Missing)
        std::this_thread::sleep_for(100ms);
    return completed.load(std::memory_order_acquire);
}

} // namespace

int main(int argc, char** argv) {
    if (kStandalone[0] != 'P' || kShipping[0] != 'P' || kProfile[0] != 'P' || kManifest[0] != 'P' ||
        kCapability[0] != 'P')
        return 73;
    for (int index = 0; index < argc; ++index)
        if (contains_authority_material(argv[index]))
            return 71;
#ifdef _WIN32
    auto environment = _environ;
#else
    auto environment = environ;
#endif
    for (; environment && *environment; ++environment)
        if (contains_authority_material(*environment))
            return 72;

    const std::string_view mode = argc > 1 ? argv[1] : "--normal";
    if (mode == "--exit")
        return 0;
    if (mode == "--stall") {
        std::this_thread::sleep_for(10s);
        return 0;
    }
    if (mode == "--verbose") {
        const std::string output = std::string(256u * 1024u, 'v') + "\n";
        std::cout << output << std::flush;
    }
    if (mode == "--delayed")
        std::this_thread::sleep_for(200ms);

    const auto handle = pulp::inspect::inherited_control_host_bootstrap_handle();
#ifdef _WIN32
    if (handle == nullptr)
#else
    if (handle < 0)
#endif
        return 65;

    if (mode == "--wrong-nonce")
        return send_wrong_nonce(handle) ? 0 : 66;
    if (mode == "--malformed")
        return send_malformed(handle) ? 0 : 67;
    if (mode == "--missing-receipt")
        return finish_without_valid_receipt(handle, ReceiptMode::Missing) ? 0 : 74;
    if (mode == "--wrong-receipt-nonce")
        return finish_without_valid_receipt(handle, ReceiptMode::WrongNonce) ? 0 : 75;
    if (mode == "--replayed-preflight-response")
        return finish_without_valid_receipt(handle, ReceiptMode::ReplayedResponse) ? 0 : 76;
    if (mode != "--normal" && mode != "--unrelated-handle" && mode != "--verbose" &&
        mode != "--delayed")
        return 64;

#ifndef _WIN32
    if (mode == "--unrelated-handle") {
        if (argc != 3)
            return 68;
        const auto unrelated = std::atoi(argv[2]);
        if (::fcntl(unrelated, F_GETFD) >= 0 || errno != EBADF)
            return 69;
    }
#endif

    pulp::inspect::ControlHostPreflightDiagnostics diagnostics;
    // Keep the child-side receive window comfortably above focused parent
    // deadlines so a loaded test host cannot make the fixture abandon a valid
    // rendezvous before the launcher gets scheduled.
    auto record = pulp::inspect::receive_control_host_preflight(handle, 10s, std::nullopt,
                                                                &diagnostics);
    if (!record) {
        std::cerr << "preflight status=" << static_cast<unsigned>(diagnostics.status)
                  << " explanation=" << diagnostics.explanation << '\n';
        return 70;
    }
    std::cout << record->registration_id.value << '\n';
    return 0;
}
