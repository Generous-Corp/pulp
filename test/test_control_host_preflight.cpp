#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef PULP_CONTROL_HOST_PREFLIGHT_FIXTURE
#error "PULP_CONTROL_HOST_PREFLIGHT_FIXTURE must name the preflight child fixture"
#endif

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

ControlHostBootstrapRecord make_record() {
    ControlHostBootstrapRecord record;
    record.endpoint_path = "/tmp/pulp-control-preflight.sock";
    record.expected_broker = {.evidence = {
                                  .role = ControlPeerRole::TrustedHostBridge,
                                  .user_id = "uid:1",
                                  .process_id = 1,
                                  .process_start_id = "pidversion:1",
                                  .executable_identity = "signed:broker",
                                  .publisher_id = "publisher:broker",
                              }};
    record.admission_id = "admission-1";
    record.registration_id = ControlRegistrationId{"registration-1"};
    record.expires_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    (std::chrono::system_clock::now() + 1min).time_since_epoch())
                                    .count();
    return record;
}

struct LaunchResult {
    bool started = false;
    int actual_pid = -1;
    ControlHostPreflightDiagnostics diagnostics;
    std::optional<ControlPeerEvidence> peer;
    pulp::platform::ProcessResult process;
};

LaunchResult launch(std::string mode = "--normal", std::optional<int> expected_pid = std::nullopt,
                    bool reject_authority = false, std::string extra_argument = {},
                    std::chrono::milliseconds preflight_timeout = 8s) {
    LaunchResult result;
    pulp::platform::ProcessOptions options;
    options.standard_input_timeout_ms = static_cast<int>(preflight_timeout.count() + 1000);
    options.timeout_ms = options.standard_input_timeout_ms + 1000;
    const ControlPeerVerifier verifier([&](const ControlPeerEvidence& evidence) {
        return !reject_authority && evidence.role == ControlPeerRole::StandaloneHost &&
               evidence.process_id == result.actual_pid &&
               evidence.process_start_id.starts_with("pidversion:") &&
               evidence.executable_identity.starts_with("signed:") &&
               !evidence.publisher_id.empty();
    });
    pulp::platform::ChildProcess child;
    std::vector<std::string> arguments{std::move(mode)};
    if (!extra_argument.empty())
        arguments.push_back(std::move(extra_argument));
    result.started = child.start_with_standard_input_channel(
        PULP_CONTROL_HOST_PREFLIGHT_FIXTURE, arguments,
        [&](int actual_pid, pulp::platform::ChildProcessInputChannel channel) {
            result.actual_pid = actual_pid;
            auto verified =
                preflight_control_host(std::move(channel), expected_pid.value_or(actual_pid),
                                       ControlPeerRole::StandaloneHost, verifier,
                                       encode_control_host_bootstrap(make_record()),
                                       preflight_timeout, &result.diagnostics);
            if (verified)
                result.peer = verified->evidence();
            return verified.has_value();
        },
        options);
    result.process = child.wait();
    INFO(result.process.stderr_output);
    CHECK(child.process_id() == result.actual_pid);
    CHECK_FALSE(child.is_running());
    return result;
}

} // namespace

TEST_CASE("private preflight observes the post-exec child before releasing bootstrap",
          "[inspect][control][host][preflight][security]") {
#ifdef __APPLE__
    const auto result = launch();
    REQUIRE(result.started);
    REQUIRE(result.peer);
    CHECK(result.diagnostics.status == ControlHostPreflightStatus::Accepted);
    CHECK(result.peer->process_id == result.actual_pid);
    CHECK(result.peer->process_start_id.starts_with("pidversion:"));
    CHECK(result.process.exit_code == 0);
    CHECK(result.process.stdout_output.find("registration-1") != std::string::npos);
#else
    const auto result = launch();
    CHECK_FALSE(result.started);
    CHECK_FALSE(result.peer);
#endif
}

TEST_CASE("private preflight rejects wrong process authority nonce and malformed input",
          "[inspect][control][host][preflight][negative]") {
#ifdef __APPLE__
    pulp::platform::ChildProcess sibling;
    REQUIRE(sibling.start("/bin/sleep", {"3"}));
    const auto sibling_pid = sibling.process_id();
    REQUIRE(sibling_pid > 0);
    auto wrong_process = launch("--normal", sibling_pid);
    CHECK_FALSE(wrong_process.started);
    CHECK(wrong_process.diagnostics.status == ControlHostPreflightStatus::ProcessMismatch);
    CHECK((wrong_process.process.was_cancelled || wrong_process.process.exit_code != 0));
    sibling.cancel();
    CHECK_FALSE(sibling.is_running());

    auto parent_bound = launch("--normal", static_cast<int>(::getpid()));
    CHECK_FALSE(parent_bound.started);
    CHECK(parent_bound.diagnostics.status == ControlHostPreflightStatus::ProcessMismatch);

    auto rejected = launch("--normal", std::nullopt, true);
    CHECK_FALSE(rejected.started);
    CHECK(rejected.diagnostics.status == ControlHostPreflightStatus::AuthorityRejected);

    auto wrong_nonce = launch("--wrong-nonce");
    CHECK_FALSE(wrong_nonce.started);
    CHECK(wrong_nonce.diagnostics.status == ControlHostPreflightStatus::NonceMismatch);

    auto malformed = launch("--malformed");
    CHECK_FALSE(malformed.started);
    CHECK(malformed.diagnostics.status == ControlHostPreflightStatus::MalformedMessage);

#else
    SUCCEED("unsupported peer-verification platforms fail before authority release");
#endif
}

TEST_CASE("private preflight timeout and child exit are bounded and joined",
          "[inspect][control][host][preflight][lifecycle]") {
#ifdef __APPLE__
    const auto started_at = std::chrono::steady_clock::now();
    auto stalled = launch("--stall", std::nullopt, false, {}, 300ms);
    CHECK_FALSE(stalled.started);
    CHECK(stalled.diagnostics.status == ControlHostPreflightStatus::Timeout);
    CHECK(stalled.process.was_cancelled);
    CHECK(std::chrono::steady_clock::now() - started_at < 2s);

    auto exited = launch("--exit");
    CHECK_FALSE(exited.started);
    CHECK(exited.diagnostics.status == ControlHostPreflightStatus::Timeout);
#else
    SUCCEED("unsupported child channel platforms fail closed before spawn");
#endif
}

TEST_CASE("private preflight child inherits no unrelated descriptor",
          "[inspect][control][host][preflight][handles]") {
#ifdef __APPLE__
    int sentinel[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sentinel) == 0);
    const auto flags = ::fcntl(sentinel[0], F_GETFD);
    REQUIRE(flags >= 0);
    REQUIRE(::fcntl(sentinel[0], F_SETFD, flags & ~FD_CLOEXEC) == 0);

    auto result = launch("--unrelated-handle", std::nullopt, false, std::to_string(sentinel[0]));
    ::close(sentinel[0]);
    ::close(sentinel[1]);
    CHECK(result.started);
#else
    SUCCEED("Windows channel sessions are rejected before CreateProcess");
#endif
}
