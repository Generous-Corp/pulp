#include <catch2/catch_test_macros.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_host_router.hpp>

#include <pulp/runtime/crypto.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <signal.h>
#include <thread>
#include <tuple>

#ifdef __APPLE__
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {

constexpr std::string_view kManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-trusted-host-fixture",
  "product_name": "Pulp Trusted Host Fixture",
  "bundle_id": "dev.pulp.test.trusted-host-fixture",
  "build_id": "build:0123456789abcdef0123456789abcdef",
  "registry_digest": "55d8e72427d276ffa6ccf744eecd15284f9844c17afd0e02bbebc89ea8cb8433",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.instance/read@1"]
}
)";

struct Directory {
    Directory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
               ("pe-" + pulp::runtime::hex_encode(*random));
        std::filesystem::create_directories(root / "source");
        std::filesystem::create_directories(root / "stage");
        std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        std::filesystem::permissions(root / "source", std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        std::filesystem::permissions(root / "stage", std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

#ifdef __APPLE__
void write_manifest(const std::filesystem::path& executable) {
    std::ofstream output(executable.string() + ".inspector-capabilities.json");
    output << kManifest;
    output.close();
    std::filesystem::permissions(executable.string() + ".inspector-capabilities.json",
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
}

std::optional<ControlTrustedHostSnapshot> make_snapshot(Directory& directory) {
    const auto source = directory.root / "source" / "endpoint-host";
    std::filesystem::copy_file(PULP_CONTROL_ENROLLMENT_HOST_FIXTURE, source,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::permissions(source,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    write_manifest(source);
    std::ifstream binary_input(source, std::ios::binary);
    const std::string binary{std::istreambuf_iterator<char>(binary_input), {}};
    const auto validation = validate_control_artifact_bytes(
        binary,
        {.profile_id = "developer-local",
         .manifest_digest = "6b86182fb49422a5cbd8c43a6a577daedf7a4700f0e5ec2e3f913c290f1799fb",
         .endpoint_included = true,
         .capability_ids = {"session.describe"}});
    CAPTURE(validation.error);
    REQUIRE(validation.valid);
    ControlTrustedHostInventory inventory(
        {.staging_root = directory.root / "stage", .broker_generation = 17, .ttl = 10s});
    const auto prepared = inventory.prepare({.executable = source,
                                             .working_directory = directory.root / "source",
                                             .host_tier = ControlHostTier::Standalone});
    CAPTURE(control_trusted_host_inventory_status_id(prepared.status));
    REQUIRE(prepared.ticket);
    return inventory.consume(prepared.ticket->inventory_id);
}

ControlHostEnrollmentTicket make_enrollment(ControlHostEnrollmentStore& store, Directory& directory,
                                            const VerifiedControlPeerIdentity& expected_peer) {
    auto snapshot = make_snapshot(directory);
    REQUIRE(snapshot);
    const auto result = store.create(std::move(*snapshot), expected_peer, 17,
                                     std::chrono::steady_clock::now() + 5s);
    REQUIRE(result.status == ControlHostEnrollmentStatus::Created);
    REQUIRE(result.ticket);
    return *result.ticket;
}

struct SpawnedHost {
    pid_t pid = -1;
    int input = -1;

    SpawnedHost() {
        int pipe_fds[2]{};
        REQUIRE(::pipe(pipe_fds) == 0);
        pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::close(pipe_fds[1]);
            ::dup2(pipe_fds[0], STDIN_FILENO);
            ::close(pipe_fds[0]);
            ::execl(PULP_CONTROL_ENROLLMENT_HOST_FIXTURE, PULP_CONTROL_ENROLLMENT_HOST_FIXTURE,
                    nullptr);
            ::_exit(127);
        }
        ::close(pipe_fds[0]);
        input = pipe_fds[1];
    }

    VerifiedControlPeerIdentity observe(Directory& directory) {
        InterprocessConnectionServer observer;
        std::mutex mutex;
        std::condition_variable ready;
        std::unique_ptr<InterprocessConnection> accepted;
        observer.on_client_connected = [&](std::unique_ptr<InterprocessConnection> connection) {
            {
                std::lock_guard lock(mutex);
                accepted = std::move(connection);
            }
            ready.notify_all();
        };
        const auto endpoint = directory.root / ("preflight-" + std::to_string(pid) + ".sock");
        REQUIRE(observer.start(endpoint.string(), IpcTransport::LocalSocket));
        const auto payload = endpoint.string() + "\n";
        REQUIRE(::write(input, payload.data(), payload.size()) ==
                static_cast<ssize_t>(payload.size()));
        {
            std::unique_lock lock(mutex);
            REQUIRE(ready.wait_for(lock, 2s, [&] { return accepted != nullptr; }));
        }
        const auto evidence = observe_control_peer(*accepted, ControlPeerRole::StandaloneHost);
        REQUIRE(evidence);
        auto verified = verify_control_peer(*accepted, {.evidence = *evidence});
        REQUIRE(verified);
        accepted->disconnect();
        accepted.reset();
        observer.stop();
        return std::move(*verified);
    }

    void start(const std::filesystem::path& endpoint, std::string_view enrollment_id,
               const std::filesystem::path& result, const std::filesystem::path& stop = {},
               std::string_view mode = "single") {
        const auto payload = endpoint.string() + "\n" + std::string(enrollment_id) + "\n" +
                             result.string() + "\n" + stop.string() + "\n" + std::string(mode) +
                             "\n";
        REQUIRE(::write(input, payload.data(), payload.size()) ==
                static_cast<ssize_t>(payload.size()));
        ::close(input);
        input = -1;
    }

    std::tuple<unsigned, unsigned, std::string> wait_result(const std::filesystem::path& path) {
        for (unsigned attempt = 0; attempt < 3000 && !std::filesystem::exists(path); ++attempt)
            std::this_thread::sleep_for(1ms);
        std::ifstream input_file(path);
        unsigned accepted = 0;
        unsigned denied = 0;
        std::string registration;
        input_file >> accepted >> denied >> registration;
        return {accepted, denied, registration};
    }

    int wait() {
        int status = 0;
        REQUIRE(::waitpid(pid, &status, 0) == pid);
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
    }

    ~SpawnedHost() {
        if (input >= 0)
            ::close(input);
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int ignored = 0;
            (void)::waitpid(pid, &ignored, 0);
        }
    }
};

struct RawHost {
    InterprocessConnection connection;
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<ControlHostOpenResult> result;

    bool connect(const std::filesystem::path& endpoint) {
        connection.set_max_message_bytes(kControlMaximumEnvelopeBytes);
        connection.set_frame_read_timeout(2s);
        connection.set_write_timeout(2s);
        connection.set_on_message([this](const void* data, std::size_t size) {
            const auto envelope =
                decode_control_envelope(std::string_view(static_cast<const char*>(data), size));
            std::optional<ControlHostOpenResult> opened;
            if (envelope)
                if (const auto* value = std::get_if<ControlHostOpenResult>(&envelope->payload))
                    opened = *value;
            {
                std::lock_guard lock(mutex);
                if (opened)
                    result = std::move(opened);
            }
            ready.notify_all();
        });
        return connection.connect(endpoint.string(), IpcTransport::LocalSocket, 2s);
    }

    std::optional<ControlHostOpenResult> open(std::string enrollment_id) {
        const auto encoded = encode_control_envelope(ControlEnvelope{
            .payload = ControlHostOpenEnvelope{.request_id = "enroll-open",
                                               .enrollment_id = std::move(enrollment_id)}});
        if (encoded.empty() || !connection.send_message(encoded))
            return std::nullopt;
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 2s, [&] { return result.has_value(); }))
            return std::nullopt;
        return result;
    }
};

struct EndpointFixture {
    Directory directory;
    ControlBroker broker{{.identities = {.max_registrations = 1}}};
    ControlHostRouter router;
    ControlService service{broker, router.executor()};
    ControlHostEnrollmentStore enrollments;
    ControlConnectionAdmissionStore admissions;
    ControlEndpointEnrollmentContext context{enrollments, broker, admissions};
    ControlEndpoint endpoint{
        service,
        [](std::string_view) -> std::optional<ControlConnectionAdmission> { return std::nullopt; },
        {.endpoint_path = directory.root / "broker.sock",
         .sdk_version = "0.791.0-test",
         .broker_id = broker.broker_id().value,
         .process_generation = 17},
        &router,
        &context};
};
#endif

} // namespace

TEST_CASE("endpoint enrollment registers attaches and rolls back on disconnect",
          "[inspect][control][endpoint][enrollment]") {
#ifdef __APPLE__
    EndpointFixture fixture;
    SpawnedHost host;
    const auto host_peer = host.observe(fixture.directory);
    const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, host_peer);
    REQUIRE(fixture.endpoint.start());
    const auto result_path = fixture.directory.root / "result";
    const auto stop_path = fixture.directory.root / "stop";
    host.start(fixture.directory.root / "broker.sock", ticket.enrollment_id, result_path,
               stop_path);
    const auto [accepted, denied, registration_value] = host.wait_result(result_path);
    REQUIRE(accepted == 1);
    CHECK(denied == 0);
    const ControlRegistrationId registration{registration_value};
    CHECK(fixture.router.connected(registration));

    std::ofstream(stop_path) << "stop";
    CHECK(host.wait() == 0);
    for (unsigned attempt = 0; attempt < 200 && fixture.router.connected(registration); ++attempt)
        std::this_thread::sleep_for(1ms);
    CHECK_FALSE(fixture.router.connected(registration));

    // Capacity one proves disconnect removed broker identity, not only routing.
    SpawnedHost replacement;
    const auto replacement_peer = replacement.observe(fixture.directory);
    const auto replacement_ticket =
        make_enrollment(fixture.enrollments, fixture.directory, replacement_peer);
    const auto replacement_result = fixture.directory.root / "replacement-result";
    const auto replacement_stop = fixture.directory.root / "replacement-stop";
    replacement.start(fixture.directory.root / "broker.sock", replacement_ticket.enrollment_id,
                      replacement_result, replacement_stop);
    const auto [replacement_accepted, replacement_denied, replacement_registration] =
        replacement.wait_result(replacement_result);
    CHECK(replacement_accepted == 1);
    CHECK(replacement_denied == 0);
    CHECK_FALSE(replacement_registration.empty());
    std::ofstream(replacement_stop) << "stop";
    CHECK(replacement.wait() == 0);
    fixture.endpoint.stop();
#endif
}

TEST_CASE("endpoint enrollment is null and denied by default",
          "[inspect][control][endpoint][enrollment][security]") {
#ifdef __APPLE__
    EndpointFixture fixture;
    SpawnedHost expected;
    const auto expected_peer = expected.observe(fixture.directory);
    const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, expected_peer);
    ControlEndpoint disabled{
        fixture.service,
        [](std::string_view) -> std::optional<ControlConnectionAdmission> { return std::nullopt; },
        {.endpoint_path = fixture.directory.root / "disabled.sock",
         .sdk_version = "0.791.0-test",
         .broker_id = fixture.broker.broker_id().value,
         .process_generation = 17},
        &fixture.router};
    REQUIRE(disabled.start());
    RawHost host;
    REQUIRE(host.connect(fixture.directory.root / "disabled.sock"));
    const auto opened = host.open(ticket.enrollment_id);
    REQUIRE(opened);
    CHECK_FALSE(opened->accepted);
    CHECK(opened->error_code == "enrollment-denied");
    CHECK(fixture.enrollments.size() == 1);
    disabled.stop();
#endif
}

TEST_CASE("endpoint burns wrong PID identity and stale generation claims",
          "[inspect][control][endpoint][enrollment][security]") {
#ifdef __APPLE__
    SECTION("same binary wrong sibling PID") {
        EndpointFixture fixture;
        SpawnedHost expected;
        SpawnedHost sibling;
        const auto expected_peer = expected.observe(fixture.directory);
        const auto sibling_peer = sibling.observe(fixture.directory);
        (void)sibling_peer;
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, expected_peer);
        REQUIRE(fixture.endpoint.start());
        const auto sibling_result = fixture.directory.root / "sibling-result";
        sibling.start(fixture.directory.root / "broker.sock", ticket.enrollment_id, sibling_result);
        const auto [sibling_accepted, sibling_denied, sibling_registration] =
            sibling.wait_result(sibling_result);
        CHECK(sibling_accepted == 0);
        CHECK(sibling_denied == 1);
        CHECK(sibling_registration.empty());
        CHECK(sibling.wait() == 2);
        CHECK(fixture.enrollments.size() == 0);
        const auto expected_result = fixture.directory.root / "expected-result";
        expected.start(fixture.directory.root / "broker.sock", ticket.enrollment_id,
                       expected_result);
        const auto [expected_accepted, expected_denied, expected_registration] =
            expected.wait_result(expected_result);
        CHECK(expected_accepted == 0);
        CHECK(expected_denied == 1);
        CHECK(expected_registration.empty());
        CHECK(expected.wait() == 2);
        fixture.endpoint.stop();
    }
    SECTION("static identity mismatch") {
        EndpointFixture fixture;
        SpawnedHost expected;
        const auto expected_peer = expected.observe(fixture.directory);
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, expected_peer);
        REQUIRE(fixture.endpoint.start());
        RawHost host;
        REQUIRE(host.connect(fixture.directory.root / "broker.sock"));
        const auto denied = host.open(ticket.enrollment_id);
        REQUIRE(denied);
        CHECK_FALSE(denied->accepted);
        CHECK(denied->error_code == "peer-verification-failed");
        CHECK(fixture.enrollments.size() == 0);
        fixture.endpoint.stop();
    }
    SECTION("same PID stale audit pidversion") {
        EndpointFixture fixture;
        SpawnedHost expected;
        const auto observed_peer = expected.observe(fixture.directory);
        auto stale_evidence = observed_peer.evidence();
        stale_evidence.process_start_id += "-stale";
        ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
        const auto stale_peer = verifier.verify(std::move(stale_evidence));
        REQUIRE(stale_peer);
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, *stale_peer);
        REQUIRE(fixture.endpoint.start());
        const auto result_path = fixture.directory.root / "stale-pidversion-result";
        expected.start(fixture.directory.root / "broker.sock", ticket.enrollment_id, result_path);
        const auto [accepted, denied, registration] = expected.wait_result(result_path);
        CHECK(accepted == 0);
        CHECK(denied == 1);
        CHECK(registration.empty());
        CHECK(expected.wait() == 2);
        CHECK(fixture.enrollments.size() == 0);
        fixture.endpoint.stop();
    }
    SECTION("stale endpoint generation") {
        EndpointFixture fixture;
        SpawnedHost expected;
        const auto expected_peer = expected.observe(fixture.directory);
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, expected_peer);
        ControlEndpoint stale{fixture.service,
                              [](std::string_view) -> std::optional<ControlConnectionAdmission> {
                                  return std::nullopt;
                              },
                              {.endpoint_path = fixture.directory.root / "stale.sock",
                               .sdk_version = "0.791.0-test",
                               .broker_id = fixture.broker.broker_id().value,
                               .process_generation = 18},
                              &fixture.router,
                              &fixture.context};
        REQUIRE(stale.start());
        RawHost host;
        REQUIRE(host.connect(fixture.directory.root / "stale.sock"));
        const auto denied = host.open(ticket.enrollment_id);
        REQUIRE(denied);
        CHECK_FALSE(denied->accepted);
        CHECK(denied->error_code == "enrollment-denied");
        CHECK(fixture.enrollments.size() == 0);
        stale.stop();
    }
#endif
}

TEST_CASE("endpoint enrollment admits exactly one concurrent claimant",
          "[inspect][control][endpoint][enrollment][concurrency]") {
#ifdef __APPLE__
    EndpointFixture fixture;
    SpawnedHost host;
    const auto host_peer = host.observe(fixture.directory);
    const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, host_peer);
    REQUIRE(fixture.endpoint.start());
    const auto result_path = fixture.directory.root / "concurrent-result";
    const auto stop_path = fixture.directory.root / "concurrent-stop";
    host.start(fixture.directory.root / "broker.sock", ticket.enrollment_id, result_path, stop_path,
               "concurrent");
    const auto [accepted, denied, registration] = host.wait_result(result_path);
    CHECK(accepted == 1);
    CHECK(denied == 1);
    CHECK_FALSE(registration.empty());
    std::ofstream(stop_path) << "stop";
    CHECK(host.wait() == 0);
    fixture.endpoint.stop();
#endif
}

TEST_CASE("host enrollment connection rejects wrong and replayed authorities",
          "[inspect][control][endpoint][enrollment][security]") {
#ifdef __APPLE__
    SECTION("wrong enrollment identity") {
        EndpointFixture fixture;
        SpawnedHost host;
        const auto host_peer = host.observe(fixture.directory);
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, host_peer);
        REQUIRE(fixture.endpoint.start());
        const auto result_path = fixture.directory.root / "wrong-enrollment-result";
        host.start(fixture.directory.root / "broker.sock", ticket.enrollment_id + "-wrong",
                   result_path);
        const auto [accepted, denied, registration] = host.wait_result(result_path);
        CHECK(accepted == 0);
        CHECK(denied == 1);
        CHECK(registration.empty());
        CHECK(host.wait() == 2);
        CHECK(fixture.enrollments.size() == 1);
        fixture.endpoint.stop();
    }
    SECTION("second open and cross-mode replay") {
        EndpointFixture fixture;
        SpawnedHost host;
        const auto host_peer = host.observe(fixture.directory);
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, host_peer);
        REQUIRE(fixture.endpoint.start());
        const auto result_path = fixture.directory.root / "replay-result";
        const auto stop_path = fixture.directory.root / "replay-stop";
        host.start(fixture.directory.root / "broker.sock", ticket.enrollment_id, result_path,
                   stop_path, "replay");
        const auto [accepted, denied, registration] = host.wait_result(result_path);
        CHECK(accepted == 1);
        CHECK(denied == 2);
        CHECK_FALSE(registration.empty());
        CHECK(fixture.enrollments.size() == 0);
        std::ofstream(stop_path) << "stop";
        CHECK(host.wait() == 0);
        fixture.endpoint.stop();
    }
#endif
}

TEST_CASE("endpoint rolls registration back when admission or router attach fails",
          "[inspect][control][endpoint][enrollment][rollback]") {
#ifdef __APPLE__
    SECTION("admission issue") {
        EndpointFixture fixture;
        ControlConnectionAdmissionStore disabled({.maximum_admissions = 0});
        ControlEndpointEnrollmentContext disabled_context{fixture.enrollments, fixture.broker,
                                                          disabled};
        ControlEndpoint disabled_endpoint{
            fixture.service,
            [](std::string_view) -> std::optional<ControlConnectionAdmission> {
                return std::nullopt;
            },
            {.endpoint_path = fixture.directory.root / "admission.sock",
             .sdk_version = "0.791.0-test",
             .broker_id = fixture.broker.broker_id().value,
             .process_generation = 17},
            &fixture.router,
            &disabled_context};
        SpawnedHost host;
        const auto host_peer = host.observe(fixture.directory);
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, host_peer);
        REQUIRE(disabled_endpoint.start());
        const auto result_path = fixture.directory.root / "admission-result";
        host.start(fixture.directory.root / "admission.sock", ticket.enrollment_id, result_path);
        const auto [accepted, denied, registration] = host.wait_result(result_path);
        CHECK(accepted == 0);
        CHECK(denied == 1);
        CHECK(registration.empty());
        CHECK(host.wait() == 2);
        disabled_endpoint.stop();
        SpawnedHost replacement;
        const auto replacement_peer = replacement.observe(fixture.directory);
        const auto replacement_ticket =
            make_enrollment(fixture.enrollments, fixture.directory, replacement_peer);
        REQUIRE(fixture.endpoint.start());
        const auto replacement_result = fixture.directory.root / "admission-replacement";
        const auto replacement_stop = fixture.directory.root / "admission-replacement-stop";
        replacement.start(fixture.directory.root / "broker.sock", replacement_ticket.enrollment_id,
                          replacement_result, replacement_stop);
        const auto [replacement_accepted, replacement_denied, replacement_registration] =
            replacement.wait_result(replacement_result);
        CHECK(replacement_accepted == 1);
        CHECK(replacement_denied == 0);
        CHECK_FALSE(replacement_registration.empty());
        std::ofstream(replacement_stop) << "stop";
        CHECK(replacement.wait() == 0);
        fixture.endpoint.stop();
    }
    SECTION("router attach") {
        EndpointFixture fixture;
        fixture.router.stop();
        SpawnedHost host;
        const auto host_peer = host.observe(fixture.directory);
        const auto ticket = make_enrollment(fixture.enrollments, fixture.directory, host_peer);
        REQUIRE(fixture.endpoint.start());
        const auto result_path = fixture.directory.root / "router-result";
        host.start(fixture.directory.root / "broker.sock", ticket.enrollment_id, result_path);
        const auto [accepted, denied, registration] = host.wait_result(result_path);
        CHECK(accepted == 0);
        CHECK(denied == 1);
        CHECK(registration.empty());
        CHECK(host.wait() == 2);
        fixture.endpoint.stop();

        ControlHostRouter replacement_router;
        ControlService replacement_service{fixture.broker, replacement_router.executor()};
        ControlEndpoint replacement_endpoint{
            replacement_service,
            [](std::string_view) -> std::optional<ControlConnectionAdmission> {
                return std::nullopt;
            },
            {.endpoint_path = fixture.directory.root / "replacement.sock",
             .sdk_version = "0.791.0-test",
             .broker_id = fixture.broker.broker_id().value,
             .process_generation = 17},
            &replacement_router,
            &fixture.context};
        SpawnedHost replacement;
        const auto replacement_peer = replacement.observe(fixture.directory);
        const auto replacement_ticket =
            make_enrollment(fixture.enrollments, fixture.directory, replacement_peer);
        REQUIRE(replacement_endpoint.start());
        const auto replacement_result = fixture.directory.root / "router-replacement";
        const auto replacement_stop = fixture.directory.root / "router-replacement-stop";
        replacement.start(fixture.directory.root / "replacement.sock",
                          replacement_ticket.enrollment_id, replacement_result, replacement_stop);
        const auto [replacement_accepted, replacement_denied, replacement_registration] =
            replacement.wait_result(replacement_result);
        CHECK(replacement_accepted == 1);
        CHECK(replacement_denied == 0);
        CHECK_FALSE(replacement_registration.empty());
        std::ofstream(replacement_stop) << "stop";
        CHECK(replacement.wait() == 0);
        replacement_endpoint.stop();
    }
    SECTION("result send or immediate disconnect") {
        EndpointFixture fixture;
        SpawnedHost dropped;
        const auto dropped_peer = dropped.observe(fixture.directory);
        const auto dropped_ticket =
            make_enrollment(fixture.enrollments, fixture.directory, dropped_peer);
        REQUIRE(fixture.endpoint.start());
        const auto dropped_result = fixture.directory.root / "dropped-result";
        dropped.start(fixture.directory.root / "broker.sock", dropped_ticket.enrollment_id,
                      dropped_result, {}, "drop");
        CHECK(dropped.wait() == 0);
        for (unsigned attempt = 0; attempt < 500 && fixture.enrollments.size() != 0; ++attempt)
            std::this_thread::sleep_for(1ms);
        CHECK(fixture.enrollments.size() == 0);
        std::this_thread::sleep_for(20ms);

        SpawnedHost replacement;
        const auto replacement_peer = replacement.observe(fixture.directory);
        const auto replacement_ticket =
            make_enrollment(fixture.enrollments, fixture.directory, replacement_peer);
        const auto replacement_result = fixture.directory.root / "drop-replacement";
        const auto replacement_stop = fixture.directory.root / "drop-replacement-stop";
        replacement.start(fixture.directory.root / "broker.sock", replacement_ticket.enrollment_id,
                          replacement_result, replacement_stop);
        const auto [replacement_accepted, replacement_denied, replacement_registration] =
            replacement.wait_result(replacement_result);
        CHECK(replacement_accepted == 1);
        CHECK(replacement_denied == 0);
        CHECK_FALSE(replacement_registration.empty());
        std::ofstream(replacement_stop) << "stop";
        CHECK(replacement.wait() == 0);
        fixture.endpoint.stop();
    }
#endif
}
