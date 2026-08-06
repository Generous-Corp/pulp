#pragma once

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/runtime/socket.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#endif

using pulp::inspect::generate_inspector_secret;
using pulp::inspect::InspectorCapability;
using pulp::inspect::InspectorClient;
using pulp::inspect::InspectorDiscoveryPublisher;
using pulp::inspect::InspectorDiscoveryReader;
using pulp::inspect::InspectorDiscoveryRecord;
using pulp::inspect::InspectorMainThreadRpc;
using pulp::inspect::InspectorMessage;
using pulp::inspect::InspectorPolicyConfig;
using pulp::inspect::InspectorProfile;
using pulp::inspect::InspectorServer;
using pulp::inspect::InspectorServerConfig;
using pulp::inspect::InspectorSession;
using pulp::inspect::InspectorSessionInfo;
using pulp::inspect::make_response;
using pulp::runtime::Socket;
using pulp::runtime::SocketType;

static_assert(!std::is_copy_constructible_v<InspectorServer>);
static_assert(!std::is_copy_assignable_v<InspectorServer>);
static_assert(!std::is_move_constructible_v<InspectorServer>);
static_assert(!std::is_move_assignable_v<InspectorServer>);

namespace {

std::shared_ptr<InspectorMainThreadRpc> make_inline_test_main_thread_rpc() {
    return std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{},
        [](auto task) {
            task();
            return true;
        },
        [] { return false; });
}

bool start_test_inspector_server(InspectorServer& server, InspectorServerConfig config) {
    if (!config.main_thread_rpc)
        config.main_thread_rpc = make_inline_test_main_thread_rpc();
    return server.start_authenticated(std::move(config));
}

[[maybe_unused]] bool send_all(Socket& socket, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = socket.send(bytes.data() + sent, bytes.size() - sent);
        if (count <= 0)
            return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

[[maybe_unused]] bool send_frame(Socket& socket, std::string_view payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    const auto size = static_cast<std::uint32_t>(payload.size());
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(size),
        static_cast<std::uint8_t>(size >> 8),
        static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 24),
    };
    return send_all(socket, header) &&
           send_all(socket, std::span(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                      payload.size()));
}

[[maybe_unused]] std::optional<std::string> receive_frame(Socket& socket) {
    auto read_exact = [&socket](std::span<std::uint8_t> bytes) {
        std::size_t received = 0;
        while (received < bytes.size()) {
            const auto count = socket.receive(bytes.data() + received, bytes.size() - received);
            if (count <= 0)
                return false;
            received += static_cast<std::size_t>(count);
        }
        return true;
    };
    std::array<std::uint8_t, 4> header{};
    if (!read_exact(header))
        return std::nullopt;
    const auto size = static_cast<std::uint32_t>(header[0]) |
                      (static_cast<std::uint32_t>(header[1]) << 8) |
                      (static_cast<std::uint32_t>(header[2]) << 16) |
                      (static_cast<std::uint32_t>(header[3]) << 24);
    if (size > 1024u * 1024u)
        return std::nullopt;
    std::string payload(size, '\0');
    if (!read_exact(std::span(reinterpret_cast<std::uint8_t*>(payload.data()), payload.size())))
        return std::nullopt;
    return payload;
}

[[maybe_unused]] bool authenticate_raw(Socket& socket, std::span<const std::uint8_t> token) {
    const auto challenge_frame = receive_frame(socket);
    if (!challenge_frame)
        return false;
    pulp::inspect::InspectorMessage challenge_message;
    if (!pulp::inspect::decode_message(*challenge_frame, challenge_message) ||
        challenge_message.method != "Session.authChallenge")
        return false;
    pulp::inspect::InspectorAuthChallenge challenge;
    try {
        const auto params = choc::json::parse(challenge_message.params_json);
        challenge.scheme = std::string(params["scheme"].getString());
        challenge.nonce_hex = std::string(params["nonce"].getString());
        challenge.session_id = std::string(params["sessionId"].getString());
        challenge.instance_id = std::string(params["instanceId"].getString());
        challenge.publication_id = std::string(params["publicationId"].getString());
        challenge.protocol_version = std::string(params["protocolVersion"].getString());
    } catch (...) {
        return false;
    }
    const auto proof = pulp::inspect::make_inspector_auth_proof(token, challenge);
    if (!proof)
        return false;
    const auto request = pulp::inspect::make_request(1, "Session.authenticate",
                                                     std::string("{\"proof\":\"") + *proof + "\"}");
    if (!send_frame(socket, pulp::inspect::encode_message(request)))
        return false;
    const auto response_frame = receive_frame(socket);
    if (!response_frame)
        return false;
    pulp::inspect::InspectorMessage response;
    return pulp::inspect::decode_message(*response_frame, response) && response.id == 1 &&
           !response.is_error;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        std::string suffix;
        for (std::size_t index = 0; index < 8; ++index)
            suffix += "0123456789abcdef"[(*token)[index] & 0xf];
        path = std::filesystem::temp_directory_path() / ("pulp-inspector-client-test-" + suffix);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

struct AuthenticatedFixture {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher{temporary.path};
    InspectorDiscoveryReader reader{temporary.path};
    InspectorPolicyConfig policy;
    std::mutex seen_mutex;
    std::vector<InspectorMessage> seen;
    InspectorSession session;
    InspectorServer server;

    AuthenticatedFixture()
        : policy([] {
              InspectorPolicyConfig result;
              result.profile = InspectorProfile::Develop;
              result.available_capabilities = {
                  InspectorCapability::SessionDescribe, InspectorCapability::SessionControl,
                  InspectorCapability::StateRead,       InspectorCapability::StateWrite,
                  InspectorCapability::TestInput,
              };
              return result;
          }()),
          session(InspectorSessionInfo{"session-client-test", "instance-client-test",
                                       "com.pulp.client-test", "1"},
                  policy, [this](const auto& request) {
                      {
                          std::lock_guard lock(seen_mutex);
                          seen.push_back(request);
                      }
                      return make_response(request.id,
                                           request.method == "State.getParameters"
                                               ? R"({"parameters":[{"id":"gain","value":0.5}]})"
                                               : R"({"applied":true})");
                  }) {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        InspectorServerConfig config{&session, &publisher, record, *token};
        config.main_thread_rpc = make_inline_test_main_thread_rpc();
        REQUIRE(start_test_inspector_server(server, std::move(config)));
    }

    ~AuthenticatedFixture() {
        server.stop();
    }
};

#ifndef _WIN32
[[maybe_unused]] std::optional<std::string> first_non_loopback_ipv4() {
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || interfaces == nullptr)
        return std::nullopt;
    std::optional<std::string> found;
    for (auto* current = interfaces; current != nullptr; current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_addr->sa_family != AF_INET ||
            (current->ifa_flags & IFF_LOOPBACK) != 0 || (current->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        char buffer[INET_ADDRSTRLEN] = {};
        const auto* address = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) != nullptr) {
            found = std::string(buffer);
            break;
        }
    }
    freeifaddrs(interfaces);
    return found;
}
#endif

} // namespace
