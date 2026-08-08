#include <pulp/inspect/control_host_enrollment.hpp>
#include <pulp/runtime/crypto.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace pulp::inspect;

constexpr std::string_view kManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-trusted-host-fixture",
  "product_name": "Pulp Trusted Host Fixture",
  "bundle_id": "dev.pulp.test.trusted-host-fixture",
  "build_id": "build:0123456789abcdef0123456789abcdef",
  "registry_digest": "d991067d0572d4f6fb5c2facac0e6f0708a5b0fad0262b552f7cde5b352197b1",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.instance/read@1"]
}
)";

struct Fixture {
    Fixture() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        root = fs::canonical(fs::temp_directory_path()) /
               ("pulp-control-enrollment-" + pulp::runtime::hex_encode(*random));
        fs::create_directories(root / "source");
        fs::create_directories(root / "stage");
#ifndef _WIN32
        ::chmod(root.c_str(), 0700);
        ::chmod((root / "source").c_str(), 0700);
        ::chmod((root / "stage").c_str(), 0700);
#endif
        executable = root / "source" / "trusted-host";
        fs::copy_file(PULP_CONTROL_TRUSTED_HOST_FIXTURE, executable);
        std::ofstream manifest(executable.string() + ".inspector-capabilities.json");
        manifest << kManifest;
        manifest.close();
#ifndef _WIN32
        ::chmod(executable.c_str(), 0700);
        ::chmod((executable.string() + ".inspector-capabilities.json").c_str(), 0600);
#endif
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    std::optional<ControlTrustedHostSnapshot>
    snapshot(ControlHostTier tier = ControlHostTier::Standalone) {
        ControlTrustedHostInventory inventory(
            {.staging_root = root / "stage", .broker_generation = 17, .ttl = 10s});
        const auto prepared = inventory.prepare(
            {.executable = executable, .working_directory = root / "source", .host_tier = tier});
        CAPTURE(control_trusted_host_inventory_status_id(prepared.status));
        REQUIRE(prepared.ticket);
        return inventory.consume(prepared.ticket->inventory_id);
    }

    fs::path root;
    fs::path executable;
};

VerifiedControlPeerIdentity verified_for(const ControlTrustedHostSnapshot& snapshot,
                                         std::int64_t process_id = 1234,
                                         std::string executable_identity = {}) {
    const auto role = snapshot.registration().host_tier == ControlHostTier::OfflineJob
                          ? ControlPeerRole::OfflineHost
                          : ControlPeerRole::StandaloneHost;
    ControlPeerEvidence evidence{.role = role,
                                 .user_id = "uid:test",
                                 .process_id = process_id,
                                 .process_start_id = "pidversion:1",
                                 .executable_identity =
                                     executable_identity.empty()
                                         ? snapshot.static_expectation().executable_identity
                                         : std::move(executable_identity),
                                 .publisher_id = snapshot.static_expectation().publisher_id};
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    auto peer = verifier.verify(std::move(evidence));
    REQUIRE(peer);
    return std::move(*peer);
}

} // namespace

TEST_CASE("host enrollment consumes a trusted snapshot exactly once",
          "[inspect][control][enrollment][security]") {
#ifdef __APPLE__
    Fixture fixture;
    auto snapshot = fixture.snapshot();
    REQUIRE(snapshot);
    const auto staged = snapshot->executable().parent_path();
    const auto peer = verified_for(*snapshot);
    const auto expires = std::chrono::steady_clock::now() + 5s;
    ControlHostEnrollmentStore store;
    const auto created = store.create(std::move(*snapshot), peer, 17, expires);
    REQUIRE(created.status == ControlHostEnrollmentStatus::Created);
    REQUIRE(created.ticket);
    CHECK(store.size() == 1);
    CHECK(fs::exists(staged));

    auto plan = store.consume(created.ticket->enrollment_id);
    REQUIRE(plan);
    CHECK_FALSE(store.consume(created.ticket->enrollment_id));
    CHECK(plan->expected_process_id() == 1234);
    CHECK(plan->broker_generation() == 17);
    CHECK(plan->snapshot().registration().host_tier == ControlHostTier::Standalone);
    CHECK(plan->expires_at() == expires);
    plan.reset();
    CHECK_FALSE(fs::exists(staged));
#endif
}

TEST_CASE("host enrollment rejects stale generations tiers and invalid expiry",
          "[inspect][control][enrollment][security]") {
#ifdef __APPLE__
    Fixture fixture;
    const auto now = std::chrono::steady_clock::now();
    ControlHostEnrollmentStore store({}, [now] { return now; });

    auto stale = fixture.snapshot();
    REQUIRE(stale);
    const auto stale_peer = verified_for(*stale);
    CHECK(store.create(std::move(*stale), stale_peer, 18, now + 1s).status ==
          ControlHostEnrollmentStatus::InvalidRequest);

    auto mismatched = fixture.snapshot();
    REQUIRE(mismatched);
    const auto mismatched_peer = verified_for(*mismatched, 1234, "signed:wrong");
    CHECK(store.create(std::move(*mismatched), mismatched_peer, 17, now + 1s).status ==
          ControlHostEnrollmentStatus::InvalidRequest);

    auto too_long = fixture.snapshot();
    REQUIRE(too_long);
    const auto too_long_peer = verified_for(*too_long);
    CHECK(store
              .create(std::move(*too_long), too_long_peer, 17,
                      now + kControlMaximumHostEnrollmentTtl + 1ms)
              .status == ControlHostEnrollmentStatus::InvalidRequest);

#endif
}

TEST_CASE("host enrollment expiry and concurrent redemption burn the claim",
          "[inspect][control][enrollment][security][concurrency]") {
#ifdef __APPLE__
    Fixture fixture;
    auto now = std::chrono::steady_clock::now();
    ControlHostEnrollmentStore store({}, [&] { return now; });
    auto snapshot = fixture.snapshot();
    REQUIRE(snapshot);
    const auto peer = verified_for(*snapshot);
    const auto created = store.create(std::move(*snapshot), peer, 17, now + 1s);
    REQUIRE(created.ticket);

    std::atomic<unsigned> winners{0};
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < 16; ++index) {
        threads.emplace_back([&] {
            if (store.consume(created.ticket->enrollment_id))
                winners.fetch_add(1);
        });
    }
    for (auto& thread : threads)
        thread.join();
    CHECK(winners.load() == 1);

    auto expiring = fixture.snapshot();
    REQUIRE(expiring);
    const auto expiring_peer = verified_for(*expiring);
    const auto expiring_ticket = store.create(std::move(*expiring), expiring_peer, 17, now + 1s);
    REQUIRE(expiring_ticket.ticket);
    now += 1s;
    CHECK_FALSE(store.consume(expiring_ticket.ticket->enrollment_id));
    CHECK(store.size() == 0);
#endif
}

TEST_CASE("host enrollment enforces its capacity bound",
          "[inspect][control][enrollment][capacity]") {
#ifdef __APPLE__
    Fixture fixture;
    const auto now = std::chrono::steady_clock::now();
    ControlHostEnrollmentStore store({.maximum_enrollments = 1}, [now] { return now; });
    auto first = fixture.snapshot();
    REQUIRE(first);
    const auto first_peer = verified_for(*first);
    REQUIRE(store.create(std::move(*first), first_peer, 17, now + 1s).ticket);
    auto second = fixture.snapshot();
    REQUIRE(second);
    const auto second_peer = verified_for(*second, 5678);
    CHECK(store.create(std::move(*second), second_peer, 17, now + 1s).status ==
          ControlHostEnrollmentStatus::ResourceExhausted);
#endif
}
