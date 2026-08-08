#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE
#error "PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE must name the bootstrap child fixture"
#endif

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

ControlPeerEvidence synthetic_broker() {
    return {.role = ControlPeerRole::TrustedHostBridge,
            .user_id = "user-1",
            .process_id = 42,
            .process_start_id = "start-1",
            .executable_identity = "signed:broker",
            .publisher_id = "publisher-1"};
}

ControlHostBootstrapRecord make_record(
    std::filesystem::path endpoint = "/tmp/pulp-control-bootstrap.sock",
    std::chrono::system_clock::time_point expiry = std::chrono::system_clock::now() + 1min) {
    ControlHostBootstrapRecord record;
    record.endpoint_path = std::move(endpoint);
    record.expected_broker = {.evidence = synthetic_broker()};
    record.admission_id = "admission-1";
    record.registration_id = ControlRegistrationId{"registration-1"};
    record.expires_at_unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(expiry.time_since_epoch()).count();
    return record;
}

void replace_version(std::vector<std::uint8_t>& bytes) {
    std::string text(bytes.begin(), bytes.end());
    auto at = text.find("\"version\": 1");
    std::size_t offset = 11;
    if (at == std::string::npos) {
        at = text.find("\"version\":1");
        offset = 10;
    }
    REQUIRE(at != std::string::npos);
    text[at + offset] = '2';
    bytes.assign(text.begin(), text.end());
}

#ifndef _WIN32
std::optional<ControlHostBootstrapRecord> read_from_socket(std::span<const std::uint8_t> bytes,
                                                           ControlHostBootstrapDiagnostics& diag,
                                                           int* duplicate = nullptr) {
    int pair[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    if (duplicate)
        *duplicate = ::dup(pair[1]);
    REQUIRE(::write(pair[0], bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    ::close(pair[0]);
    return read_control_host_bootstrap(pair[1], 500ms, std::chrono::system_clock::now(), &diag);
}
#endif

} // namespace

TEST_CASE("control host bootstrap codec rejects malformed and stale records",
          "[inspect][control][host][bootstrap]") {
    auto record = make_record();
    auto encoded = encode_control_host_bootstrap(record);
    REQUIRE_FALSE(encoded.empty());

    ControlHostBootstrapDiagnostics diagnostics;
    auto decoded = decode_control_host_bootstrap(encoded.bytes(), std::chrono::system_clock::now(),
                                                 &diagnostics);
    REQUIRE(decoded);
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Accepted);
    CHECK(decoded->registration_id.value == "registration-1");

    auto truncated = std::vector<std::uint8_t>(encoded.bytes().begin(), encoded.bytes().end());
    truncated.pop_back();
    CHECK_FALSE(
        decode_control_host_bootstrap(truncated, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Truncated);

    std::vector<std::uint8_t> oversize(kControlHostBootstrapMaximumBytes + 1, 'x');
    CHECK_FALSE(
        decode_control_host_bootstrap(oversize, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Oversize);

    auto unknown = std::vector<std::uint8_t>(encoded.bytes().begin(), encoded.bytes().end());
    replace_version(unknown);
    CHECK_FALSE(
        decode_control_host_bootstrap(unknown, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::UnsupportedVersion);

    CHECK_FALSE(decode_control_host_bootstrap(encoded.bytes(),
                                              std::chrono::system_clock::now() + 2min,
                                              &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Expired);
}

TEST_CASE("control host bootstrap handle is consumed exactly once",
          "[inspect][control][host][bootstrap][security]") {
#ifndef _WIN32
    auto record = make_record();
    const auto encoded = encode_control_host_bootstrap(record);
    ControlHostBootstrapDiagnostics diagnostics;
    int duplicate = -1;
    auto first = read_from_socket(encoded.bytes(), diagnostics, &duplicate);
    REQUIRE(first);
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Accepted);
    auto second = read_control_host_bootstrap(duplicate, 100ms, std::chrono::system_clock::now(),
                                              &diagnostics);
    CHECK_FALSE(second);
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Truncated);

    CHECK_FALSE(
        read_control_host_bootstrap(-1, 100ms, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Absent);
#else
    SUCCEED("the inherited HANDLE path is covered by the process fixture");
#endif
}

TEST_CASE("control host bootstrap inherited input leaks no unrelated descriptor",
          "[inspect][control][host][bootstrap][process][security]") {
    auto record = make_record();
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 5000;
    options.max_output_bytes = 4u * 1024u * 1024u;

#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_handle = INVALID_HANDLE_VALUE;
    HANDLE write_handle = INVALID_HANDLE_VALUE;
    REQUIRE(CreatePipe(&read_handle, &write_handle, &attributes, 0));
    const auto unrelated = std::to_string(reinterpret_cast<std::uintptr_t>(read_handle));
#else
    int sentinel[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sentinel) == 0);
    const auto flags = ::fcntl(sentinel[0], F_GETFD);
    REQUIRE(flags >= 0);
    REQUIRE(::fcntl(sentinel[0], F_SETFD, flags & ~FD_CLOEXEC) == 0);
    const auto unrelated = std::to_string(sentinel[0]);
#endif

    auto encoded = encode_control_host_bootstrap(record);
    pulp::platform::ChildProcess child;
    REQUIRE(child.start_with_standard_input(PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE,
                                            {"--unrelated-handle", unrelated}, encoded.bytes(),
                                            options));
    const auto result = child.wait();
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output.find("registration-1") != std::string::npos);

#ifdef _WIN32
    CloseHandle(read_handle);
    CloseHandle(write_handle);
#else
    ::close(sentinel[0]);
    ::close(sentinel[1]);
#endif
}

TEST_CASE("inherited input delivery is bounded when the child does not read",
          "[inspect][control][host][bootstrap][process][security]") {
    std::vector<std::uint8_t> input(64u * 1024u * 1024u, 'x');
    pulp::platform::ProcessOptions options;
    options.standard_input_timeout_ms = 50;
    const auto started_at = std::chrono::steady_clock::now();
    pulp::platform::ChildProcess child;
    CHECK_FALSE(child.start_with_standard_input(PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE, {"--stall"},
                                                input, options));
    CHECK(std::chrono::steady_clock::now() - started_at < 1500ms);
}
