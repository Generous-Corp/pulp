#include <catch2/catch_test_macros.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_host_router.hpp>
#include <pulp/inspect/control_trusted_host_launcher.hpp>
#include <pulp/runtime/crypto.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#ifdef __APPLE__
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {
namespace fs = std::filesystem;

constexpr std::string_view kManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-trusted-host-e2e-fixture",
  "product_name": "Pulp Trusted Host E2E Fixture",
  "bundle_id": "dev.pulp.test.trusted-host-e2e-fixture",
  "build_id": "build:0123456789abcdef0123456789abcdef",
  "registry_digest": "1a3bcf207e34b79c49e32038699f3738d1d814838766e4d3f7aebaf895770ace",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.instance/read@1", "dev.pulp.session/control@1", "dev.pulp.trace/session-control@1"]
}
)";

struct Directory {
    fs::path root;
    Directory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        root =
            fs::canonical("/tmp") / ("pulp-control-t1-e2e-" + pulp::runtime::hex_encode(*random));
        for (const auto* child : {"source", "stage", "receipts"})
            fs::create_directories(root / child);
#ifdef __APPLE__
        ::chmod(root.c_str(), 0700);
        ::chmod((root / "source").c_str(), 0700);
        ::chmod((root / "stage").c_str(), 0700);
        ::chmod((root / "receipts").c_str(), 0700);
#endif
    }
    ~Directory() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

#ifdef __APPLE__
ControlPeerEvidence observe_current_process(const fs::path& endpoint, ControlPeerRole role) {
    pulp::events::InterprocessConnectionServer observer;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<pulp::events::InterprocessConnection> accepted;
    observer.on_client_connected = [&](auto connection) {
        {
            std::lock_guard lock(mutex);
            accepted = std::move(connection);
        }
        ready.notify_all();
    };
    REQUIRE(observer.start(endpoint.string(), pulp::events::IpcTransport::LocalSocket));
    pulp::events::InterprocessConnection client;
    REQUIRE(client.connect(endpoint.string(), pulp::events::IpcTransport::LocalSocket, 2s));
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return accepted != nullptr; }));
    }
    const auto evidence = observe_control_peer(*accepted, role);
    REQUIRE(evidence);
    client.disconnect();
    accepted.reset();
    observer.stop();
    return *evidence;
}

VerifiedControlPeerIdentity verify_for_broker(ControlPeerEvidence evidence) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    auto verified = verifier.verify(std::move(evidence));
    REQUIRE(verified);
    return std::move(*verified);
}

ControlAdmissionPolicy allow_t1_trace() {
    ControlAdmissionPolicy policy;
    policy.host_available = [](const auto&, const auto&) { return true; };
    policy.activated = [](const auto&, const auto&) { return true; };
    policy.policy_eligible = [](const auto&, const auto&) { return true; };
    return policy;
}

std::string wait_for_registration(const fs::path& path) {
    for (unsigned attempt = 0; attempt < 5'000 && !fs::exists(path); ++attempt)
        std::this_thread::sleep_for(1ms);
    std::ifstream input(path);
    std::string registration;
    input >> registration;
    return registration;
}
#endif
} // namespace

TEST_CASE("raw signed T1 host reaches a correlated trace receipt through canonical control",
          "[inspect][control][e2e][t1][security]") {
#ifdef __APPLE__
    Directory directory;
    const auto executable = directory.root / "source" / "trusted-host";
    fs::copy_file(PULP_CONTROL_TRUSTED_HOST_E2E_FIXTURE, executable);
    ::chmod(executable.c_str(), 0700);
    {
        std::ofstream sidecar(executable.string() + ".inspector-capabilities.json");
        sidecar << kManifest;
    }
    ::chmod((executable.string() + ".inspector-capabilities.json").c_str(), 0600);
    ControlManifestDiagnostics manifest_diagnostics;
    const auto manifest = parse_control_manifest(kManifest, &manifest_diagnostics);
    INFO(manifest_diagnostics.error);
    REQUIRE(manifest);
    REQUIRE(serialize_control_manifest(*manifest) == kManifest);

    const auto broker_evidence = observe_current_process(directory.root / "broker-observer.sock",
                                                         ControlPeerRole::TrustedHostBridge);
    ControlBrokerConfig broker_config;
    broker_config.admission = allow_t1_trace();
    broker_config.operation_store = ControlOperationStoreConfig{
        .directory = directory.root / "receipts",
    };
    ControlBroker broker(std::move(broker_config));
    ControlHostRouter router;
    ControlService service(broker, router.executor());
    ControlHostEnrollmentStore enrollments;
    ControlConnectionAdmissionStore admissions;
    ControlEndpointEnrollmentContext enrollment_context{enrollments, broker, admissions};
    ControlEndpoint endpoint(
        service, [&](std::string_view id) { return admissions.consume(id); },
        {.endpoint_path = directory.root / "broker.sock",
         .sdk_version = "0.795.2-test",
         .broker_id = broker.broker_id().value,
         .process_generation = 17},
        &router, &enrollment_context);
    REQUIRE(endpoint.start());

    const auto result_path = directory.root / "host-registration";
    const auto stop_path = directory.root / "host-stop";
    ControlTrustedHostInventory inventory(
        {.staging_root = directory.root / "stage", .broker_generation = 17, .ttl = 10s});
    const auto prepared = inventory.prepare({
        .executable = executable,
        .arguments = {result_path.string(), stop_path.string()},
        .working_directory = directory.root / "source",
        .host_tier = ControlHostTier::Standalone,
    });
    INFO(control_trusted_host_inventory_status_id(prepared.status));
    REQUIRE(prepared.ticket);

    ControlTrustedHostLauncher launcher(inventory, enrollments,
                                        {.endpoint_path = directory.root / "broker.sock",
                                         .expected_broker = {.evidence = broker_evidence},
                                         .broker_generation = 17,
                                         // A freshly copied signed fixture can spend several
                                         // seconds in dyld/code-signature validation on a busy
                                         // macOS host before it reaches the inherited channel.
                                         .preflight_timeout = 10s});
    pulp::platform::ProcessOptions process_options;
    process_options.timeout_ms = 12'000;
    auto launched = launcher.launch(prepared.ticket->inventory_id, process_options);
    INFO(launched.explanation);
    INFO(static_cast<unsigned>(launched.preflight.status));
    REQUIRE(launched.launched());

    const ControlRegistrationId registration_id{wait_for_registration(result_path)};
    REQUIRE(registration_id);
    const auto registration = broker.registration(registration_id);
    REQUIRE(registration);
    REQUIRE(router.connected(registration_id));

    const auto client_evidence =
        observe_current_process(directory.root / "client-observer.sock", ControlPeerRole::Client);
    auto client_peer = verify_for_broker(client_evidence);
    const auto bootstrap = broker.issue_bootstrap(client_peer);
    REQUIRE(bootstrap.ticket);
    const auto redeemed = broker.redeem_bootstrap(bootstrap.ticket->ticket_id,
                                                  bootstrap.ticket->secret.bytes(), client_peer);
    REQUIRE(redeemed.client);
    const auto admission =
        admissions.issue({.evidence = client_evidence},
                         ControlClientConnectionPrincipal{redeemed.client->client_id});
    REQUIRE(admission.ticket);

    ControlClientConnection connection({.endpoint_path = directory.root / "broker.sock",
                                        .expected_broker = {.evidence = broker_evidence}});
    REQUIRE(connection.connect());
    REQUIRE(connection.open_session(admission.ticket->admission_id).accepted);
    ControlClient client(connection);
    const auto negotiated = client.negotiate({.mandatory_features = {"receipts"}});
    REQUIRE(negotiated.succeeded());

    const auto granted =
        broker.issue_grant(client_peer,
                           {.client_id = redeemed.client->client_id,
                            .registration_id = registration_id,
                            .capabilities = {InspectorCapability::TraceSessionControl},
                            .ttl = 1min},
                           {.approved = true,
                            .authority = ControlConsentAuthority::TrustedPulpCli,
                            .decision_id = "t1-e2e-trace"});
    REQUIRE(granted.grant);

    ControlRequestEnvelope request{
        .request_id = "t1-trace-request",
        .client_id = redeemed.client->client_id.value,
        .registration_id = registration_id.value,
        .grant_id = granted.grant->grant_id.value,
        .instance_generation = registration->publication_id,
        .operation_id = "dev.pulp.trace/session-control@1",
        .operation_version = 1,
        .idempotency_key = "t1-trace-idempotency",
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                .count(),
        .params_json = R"({"action":"start","ring_mb":8})",
    };
    request.request_hash = *control_request_hash(request);
    const auto dispatched = client.request(request, 5s);
    REQUIRE(dispatched.succeeded());
    CHECK(dispatched.response->request_id == request.request_id);
    CHECK(dispatched.response->operation_id == request.operation_id);
    const auto persisted =
        broker.operation_receipt(ControlReceiptId{dispatched.response->receipt_id});
    REQUIRE(persisted);
    CHECK(persisted->binding.registration_id == registration_id);
    CHECK(persisted->binding.operation_id == request.operation_id);
    CHECK((dispatched.response->state == ControlReceiptState::Completed ||
           dispatched.response->result_code == ControlResultCode::NotBuilt));

    std::ofstream(stop_path) << "stop";
    connection.disconnect();
    const auto process = launched.process->wait();
    INFO(process.stderr_output);
    CHECK(process.exit_code == 0);
    endpoint.stop();
#else
    SUCCEED("raw signed T1 launch is currently macOS-only");
#endif
}
