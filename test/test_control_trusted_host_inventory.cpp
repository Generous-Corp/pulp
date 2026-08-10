#include <pulp/inspect/control_trusted_host_inventory.hpp>
#include <pulp/inspect/control_trusted_host_launcher.hpp>
#include <pulp/runtime/crypto.hpp>

#include "control_static_code_identity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using pulp::inspect::ControlHostTier;
using pulp::inspect::ControlTrustedHostInventory;
using pulp::inspect::ControlTrustedHostInventoryConfig;
using pulp::inspect::ControlTrustedHostInventoryStatus;
using pulp::inspect::ControlTrustedHostLauncher;
using pulp::inspect::ControlTrustedHostLauncherConfig;
using pulp::inspect::ControlTrustedHostLaunchIntent;
using pulp::inspect::ControlTrustedHostLaunchStatus;

#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
TEST_CASE("trusted host static identity recognizes effective hardened-runtime library validation",
          "[inspect][control][inventory][security][codesign]") {
    const auto identity = pulp::inspect::detail::inspect_static_code_identity(
        fs::path(PULP_CONTROL_TRUSTED_HOST_FIXTURE));
    REQUIRE(identity);
    CHECK(identity->library_validation);
}

TEST_CASE("trusted host platform identity recognizes only Apple-signed Rosetta runtime code",
          "[inspect][control][inventory][security][rosetta]") {
    const fs::path runtime = "/Library/Apple/usr/libexec/oah/libRosettaRuntime";
    if (!fs::exists(runtime)) {
        SUCCEED("Rosetta is not installed");
        return;
    }
    CHECK(pulp::inspect::detail::is_apple_platform_code(runtime));
    CHECK_FALSE(pulp::inspect::detail::is_apple_platform_code(
        fs::path(PULP_CONTROL_TRUSTED_HOST_FIXTURE)));
}
#endif

constexpr std::string_view kManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-trusted-host-fixture",
  "product_name": "Pulp Trusted Host Fixture",
  "bundle_id": "dev.pulp.test.trusted-host-fixture",
  "build_id": "build:0123456789abcdef0123456789abcdef",
  "registry_digest": "b3bfbc17c377a58531c0689ce961d33d43d7504c61f8db979cd1a0df678409bc",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.instance/read@1"]
}
)";

class Fixture {
  public:
    Fixture() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        root = fs::canonical(fs::temp_directory_path()) /
               ("pulp-control-inventory-" + pulp::runtime::hex_encode(*random));
        fs::create_directories(root / "source");
        fs::create_directories(root / "stage");
#ifndef _WIN32
        ::chmod(root.c_str(), 0700);
        ::chmod((root / "source").c_str(), 0700);
        ::chmod((root / "stage").c_str(), 0700);
#endif
        executable = root / "source" / "trusted-host";
        fs::copy_file(PULP_CONTROL_TRUSTED_HOST_FIXTURE, executable);
#ifndef _WIN32
        ::chmod(executable.c_str(), 0700);
#endif
        write_manifest(kManifest);
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path sidecar() const {
        return executable.parent_path() /
               (executable.filename().string() + ".inspector-capabilities.json");
    }
    void write_manifest(std::string_view bytes) const {
        std::ofstream output(sidecar(), std::ios::binary | std::ios::trunc);
        output << bytes;
#ifndef _WIN32
        output.close();
        ::chmod(sidecar().c_str(), 0600);
#endif
    }
    ControlTrustedHostLaunchIntent intent() const {
        return {.executable = executable,
                .arguments = {"--fixture", "exact"},
                .working_directory = root / "source",
                .host_tier = ControlHostTier::Standalone};
    }
    ControlTrustedHostInventoryConfig config(std::size_t maximum = 64) const {
        return {.staging_root = root / "stage",
                .broker_generation = 17,
                .maximum_entries = maximum,
                .ttl = 10s};
    }

    fs::path root;
    fs::path executable;
};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

} // namespace

TEST_CASE("trusted host launcher releases enrollment only after exact child preflight",
          "[inspect][control][inventory][launcher][security]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    Fixture fixture;
    fs::copy_file(PULP_CONTROL_HOST_PREFLIGHT_FIXTURE, fixture.executable,
                  fs::copy_options::overwrite_existing);
    ::chmod(fixture.executable.c_str(), 0700);

    ControlTrustedHostInventory inventory(fixture.config());
    auto intent = fixture.intent();
    intent.arguments = {"--normal"};
    const auto prepared = inventory.prepare(intent);
    INFO(pulp::inspect::control_trusted_host_inventory_status_id(prepared.status));
    REQUIRE(prepared.ticket);

    pulp::inspect::ControlHostEnrollmentStore enrollments;
    ControlTrustedHostLauncher launcher(
        inventory, enrollments,
        ControlTrustedHostLauncherConfig{
            .endpoint_path = fixture.root / "broker.sock",
            .expected_broker = {.evidence =
                                    {
                                        .role = pulp::inspect::ControlPeerRole::TrustedHostBridge,
                                        .user_id = "uid:" + std::to_string(::getuid()),
                                        .process_id = static_cast<std::int64_t>(::getpid()),
                                        .process_start_id = "pidversion:test",
                                        .executable_identity = "signed:test-broker",
                                        .publisher_id = "publisher:test-broker",
                                    }},
            .broker_generation = 17,
            .preflight_timeout = 5s,
        });
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 8'000;
    auto launched = launcher.launch(prepared.ticket->inventory_id, options);
    INFO(launched.explanation);
    INFO(static_cast<unsigned>(launched.preflight.status));
    REQUIRE(launched.status == ControlTrustedHostLaunchStatus::Launched);
    REQUIRE(launched.process);
    CHECK(enrollments.size() == 1);
    const auto process = launched.process->wait();
    INFO(process.stderr_output);
    CHECK(process.exit_code == 0);

    const auto replay = launcher.launch(prepared.ticket->inventory_id, options);
    CHECK(replay.status == ControlTrustedHostLaunchStatus::InventoryUnavailable);

    intent.arguments = {"--exit"};
    const auto exited = inventory.prepare(intent);
    REQUIRE(exited.ticket);
    auto denied = launcher.launch(exited.ticket->inventory_id, options);
    CHECK(denied.status == ControlTrustedHostLaunchStatus::PreflightRejected);
    CHECK(enrollments.size() == 1);

    intent.arguments = {"--normal"};
    intent.host_tier = ControlHostTier::OfflineJob;
    const auto offline = inventory.prepare(intent);
    REQUIRE(offline.ticket);
    auto offline_launched = launcher.launch(offline.ticket->inventory_id, options);
    INFO(offline_launched.explanation);
    REQUIRE(offline_launched.launched());
    CHECK(enrollments.size() >= 1);
    CHECK(offline_launched.process->wait().exit_code == 0);
#endif
}

TEST_CASE("trusted host launcher revalidates staged launch material at spawn",
          "[inspect][control][inventory][launcher][security][toctou]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    Fixture fixture;
    fs::copy_file(PULP_CONTROL_HOST_PREFLIGHT_FIXTURE, fixture.executable,
                  fs::copy_options::overwrite_existing);
    ::chmod(fixture.executable.c_str(), 0700);

    ControlTrustedHostInventory inventory(fixture.config());
    auto intent = fixture.intent();
    intent.arguments = {"--normal"};
    const auto prepared = inventory.prepare(intent);
    REQUIRE(prepared.ticket);

    const auto generation = fixture.root / "stage" / "generation-17";
    const auto snapshot_directory = fs::directory_iterator(generation)->path();
    const auto staged_executable = snapshot_directory / fixture.executable.filename();
    ::chmod(staged_executable.c_str(), 0700);
    {
        std::ofstream output(staged_executable, std::ios::binary | std::ios::app);
        output << "replaced-after-inventory";
    }
    ::chmod(staged_executable.c_str(), 0500);

    pulp::inspect::ControlHostEnrollmentStore enrollments;
    ControlTrustedHostLauncher launcher(
        inventory, enrollments,
        ControlTrustedHostLauncherConfig{
            .endpoint_path = fixture.root / "broker.sock",
            .expected_broker = {.evidence =
                                    {
                                        .role = pulp::inspect::ControlPeerRole::TrustedHostBridge,
                                        .user_id = "uid:" + std::to_string(::getuid()),
                                        .process_id = static_cast<std::int64_t>(::getpid()),
                                        .process_start_id = "pidversion:test",
                                        .executable_identity = "signed:test-broker",
                                        .publisher_id = "publisher:test-broker",
                                    }},
            .broker_generation = 17,
            .preflight_timeout = 5s,
        });
    auto launched = launcher.launch(prepared.ticket->inventory_id);
    CHECK(launched.status == ControlTrustedHostLaunchStatus::InventoryUnavailable);
    CHECK_FALSE(launched.process);
    CHECK(enrollments.size() == 0);
#endif
}

TEST_CASE("trusted host inventory fails closed with invalid configuration",
          "[inspect][control][inventory][security]") {
    ControlTrustedHostInventory inventory(
        {.staging_root = "relative-stage", .broker_generation = 17});
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    CHECK(inventory.prepare({}).status == ControlTrustedHostInventoryStatus::InvalidRequest);
#else
    CHECK(inventory.prepare({}).status == ControlTrustedHostInventoryStatus::PlatformUnavailable);
#endif
    CHECK(inventory.size() == 0);
}

TEST_CASE("trusted host inventory reaps only safe startup debris",
          "[inspect][control][inventory][restart][security]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    SECTION("prior generations and incomplete current snapshots are reaped") {
        Fixture fixture;
        const auto stale_generation = fixture.root / "stage" / "generation-16";
        const auto stale_snapshot = stale_generation / "snapshot-crashed";
        const auto current_generation = fixture.root / "stage" / "generation-17";
        const auto incomplete = current_generation /
                                "snapshot-0123456789abcdef0123456789abcdef.new";
        const auto live = current_generation /
                          "snapshot-fedcba9876543210fedcba9876543210";
        fs::create_directories(stale_snapshot);
        fs::create_directories(incomplete);
        fs::create_directories(live);
        std::ofstream(stale_snapshot / "host") << "stale";
        std::ofstream(incomplete / "host") << "partial";
        std::ofstream(live / "host") << "live";
        ::chmod(stale_generation.c_str(), 0700);
        ::chmod(stale_snapshot.c_str(), 0700);
        ::chmod((stale_snapshot / "host").c_str(), 0600);
        ::chmod(current_generation.c_str(), 0700);
        ::chmod(incomplete.c_str(), 0700);
        ::chmod((incomplete / "host").c_str(), 0600);
        ::chmod(live.c_str(), 0700);
        ::chmod((live / "host").c_str(), 0600);

        ControlTrustedHostInventory inventory(fixture.config());
        CHECK_FALSE(fs::exists(stale_generation));
        CHECK_FALSE(fs::exists(incomplete));
        CHECK(fs::exists(live / "host"));
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::Prepared);
    }

    SECTION("unsafe stale generation is preserved and startup fails closed") {
        Fixture fixture;
        const auto outside = fixture.root / "outside";
        fs::create_directory(outside);
        std::ofstream(outside / "sentinel") << "preserve";
        ::chmod(outside.c_str(), 0700);
        ::chmod((outside / "sentinel").c_str(), 0600);
        const auto unsafe = fixture.root / "stage" / "generation-16";
        fs::create_directory_symlink(outside, unsafe);

        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(fs::is_symlink(unsafe));
        CHECK(read_file(outside / "sentinel") == "preserve");
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::InvalidRequest);
    }

    SECTION("directory-shaped cleanup name on a regular file is preserved") {
        Fixture fixture;
        const auto current = fixture.root / "stage" / "generation-17";
        fs::create_directory(current);
        ::chmod(current.c_str(), 0700);
        const auto unsafe = current /
                            "snapshot-0123456789abcdef0123456789abcdef.new";
        std::ofstream(unsafe) << "not-a-directory";
        ::chmod(unsafe.c_str(), 0600);

        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(read_file(unsafe) == "not-a-directory");
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::InvalidRequest);
    }
#endif
}

TEST_CASE("trusted host inventory snapshots and consumes an exact raw executable once",
          "[inspect][control][inventory][security]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    Fixture fixture;
    pulp::inspect::ControlManifestDiagnostics manifest_diagnostics;
    const auto parsed_manifest =
        pulp::inspect::parse_control_manifest(kManifest, &manifest_diagnostics);
    CAPTURE(manifest_diagnostics.error);
    REQUIRE(parsed_manifest);
    REQUIRE(pulp::inspect::serialize_control_manifest(*parsed_manifest) == kManifest);
    ControlTrustedHostInventory inventory(fixture.config());
    const auto prepared = inventory.prepare(fixture.intent());
    REQUIRE(prepared.status == ControlTrustedHostInventoryStatus::Prepared);
    REQUIRE(prepared.ticket);
    REQUIRE(inventory.size() == 1);

    auto snapshot = inventory.consume(prepared.ticket->inventory_id);
    REQUIRE(snapshot);
    CHECK_FALSE(inventory.consume(prepared.ticket->inventory_id));
    CHECK(snapshot->broker_generation() == 17);
    CHECK(snapshot->arguments() == std::vector<std::string>{"--fixture", "exact"});
    CHECK(snapshot->working_directory() == fixture.root / "source");
    CHECK(snapshot->registration().host_tier == ControlHostTier::Standalone);
    CHECK(snapshot->registration().manifest.build_id == "build:0123456789abcdef0123456789abcdef");
    CHECK(snapshot->registration().artifact_digest ==
          pulp::runtime::sha256_hex(read_file(snapshot->executable())));
    CHECK(snapshot->static_expectation().executable_identity.starts_with("signed:"));
    CHECK_FALSE(snapshot->executable().string().find(prepared.ticket->inventory_id) !=
                std::string::npos);

    const auto staged_bytes = read_file(snapshot->executable());
    const auto staged_manifest =
        read_file(snapshot->executable().parent_path() /
                  (snapshot->executable().filename().string() + ".inspector-capabilities.json"));
    CHECK(staged_bytes.find(prepared.ticket->inventory_id) == std::string::npos);
    CHECK(staged_manifest.find(snapshot->registration().session_id) == std::string::npos);
    CHECK(staged_manifest.find(snapshot->registration().publication_id) == std::string::npos);
    const auto consumed_directory = snapshot->executable().parent_path();
    snapshot.reset();
    CHECK_FALSE(fs::exists(consumed_directory));

    auto offline_intent = fixture.intent();
    offline_intent.host_tier = ControlHostTier::OfflineJob;
    const auto offline_prepared = inventory.prepare(offline_intent);
    REQUIRE(offline_prepared.ticket);
    const auto offline_snapshot = inventory.consume(offline_prepared.ticket->inventory_id);
    REQUIRE(offline_snapshot);
    CHECK(offline_snapshot->registration().host_tier == ControlHostTier::OfflineJob);
#endif
}

TEST_CASE("trusted host inventory isolates snapshots from source mutation",
          "[inspect][control][inventory][toctou]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    Fixture fixture;
    ControlTrustedHostInventory inventory(fixture.config());
    const auto prepared = inventory.prepare(fixture.intent());
    REQUIRE(prepared.ticket);
    auto snapshot = inventory.consume(prepared.ticket->inventory_id);
    REQUIRE(snapshot);
    const auto staged_before = read_file(snapshot->executable());
    {
        std::ofstream mutate(fixture.executable, std::ios::binary | std::ios::app);
        mutate << "source-mutated";
    }
    fixture.write_manifest("{}");
    CHECK(read_file(snapshot->executable()) == staged_before);
    CHECK(snapshot->registration().artifact_digest == pulp::runtime::sha256_hex(staged_before));
#endif
}

TEST_CASE("trusted host inventory rejects unsafe and unsupported carriers",
          "[inspect][control][inventory][security]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    SECTION("executable symlink") {
        Fixture fixture;
        const auto link = fixture.root / "source" / "link";
        fs::create_symlink(fixture.executable, link);
        auto intent = fixture.intent();
        intent.executable = link;
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(intent).status == ControlTrustedHostInventoryStatus::UnsafePath);
    }
    SECTION("source parent symlink") {
        Fixture fixture;
        const auto real_source = fixture.root / "real-source";
        fs::rename(fixture.root / "source", real_source);
        fs::create_directory_symlink(real_source, fixture.root / "source");
        auto intent = fixture.intent();
        intent.working_directory = real_source;
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(intent).status == ControlTrustedHostInventoryStatus::UnsafePath);
    }
    SECTION("manifest symlink") {
        Fixture fixture;
        const auto real = fixture.root / "real-manifest";
        fs::rename(fixture.sidecar(), real);
        fs::create_symlink(real, fixture.sidecar());
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::UnsafePath);
    }
    SECTION("staging root symlink") {
        Fixture fixture;
        const auto real_stage = fixture.root / "real-stage";
        fs::create_directory(real_stage);
        fs::remove(fixture.root / "stage");
        fs::create_directory_symlink(real_stage, fixture.root / "stage");
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::InvalidRequest);
    }
    SECTION("staging root is not owner private") {
        Fixture fixture;
        ::chmod((fixture.root / "stage").c_str(), 0755);
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::InvalidRequest);
    }
    SECTION("hard-linked executable") {
        Fixture fixture;
        fs::create_hard_link(fixture.executable, fixture.root / "source" / "second-link");
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::UnsafePath);
    }
    SECTION("hard-linked manifest") {
        Fixture fixture;
        fs::create_hard_link(fixture.sidecar(), fixture.root / "second-manifest-link");
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::UnsafePath);
    }
    SECTION("bundle carriers and shared plugin tier") {
        Fixture fixture;
        ControlTrustedHostInventory inventory(fixture.config());
        for (const std::string_view extension : {".app", ".APP", ".appex", ".component", ".vst3",
                                                 ".VST3", ".clap", ".lv2", ".aaxplugin"}) {
            auto bundle_intent = fixture.intent();
            bundle_intent.executable = fixture.root / ("Host" + std::string(extension));
            CHECK(inventory.prepare(bundle_intent).status ==
                  ControlTrustedHostInventoryStatus::UnsupportedArtifact);
        }
        auto directory_intent = fixture.intent();
        directory_intent.executable = fixture.root / "source";
        CHECK(inventory.prepare(directory_intent).status ==
              ControlTrustedHostInventoryStatus::UnsafePath);
        auto shared_intent = fixture.intent();
        shared_intent.host_tier = ControlHostTier::SharedPluginHost;
        CHECK(inventory.prepare(shared_intent).status ==
              ControlTrustedHostInventoryStatus::UnsupportedArtifact);
        auto unknown_intent = fixture.intent();
        unknown_intent.host_tier = static_cast<ControlHostTier>(255);
        CHECK(inventory.prepare(unknown_intent).status ==
              ControlTrustedHostInventoryStatus::InvalidRequest);
    }
#endif
}

TEST_CASE("trusted host inventory validates staged manifest markers and signature",
          "[inspect][control][inventory][security]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    SECTION("noncanonical manifest") {
        Fixture fixture;
        fixture.write_manifest(std::string(kManifest) + "\n");
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::ManifestInvalid);
    }
    SECTION("manifest digest mismatch") {
        Fixture fixture;
        auto changed = std::string(kManifest);
        changed.replace(changed.find("0123456789abcdef0123456789abcdef"), 32,
                        "1123456789abcdef0123456789abcdef");
        fixture.write_manifest(changed);
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::ArtifactInvalid);
    }
    SECTION("tampered signature") {
        Fixture fixture;
        std::ofstream output(fixture.executable, std::ios::binary | std::ios::app);
        output << "tampered";
        output.close();
        ControlTrustedHostInventory inventory(fixture.config());
        CHECK(inventory.prepare(fixture.intent()).status ==
              ControlTrustedHostInventoryStatus::SignatureInvalid);
        CHECK(fs::is_empty(fixture.root / "stage" / "generation-17"));
    }
#endif
}

TEST_CASE("trusted host inventory bounds concurrency and expires unused snapshots",
          "[inspect][control][inventory][concurrency]") {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    Fixture fixture;
    auto now = std::chrono::steady_clock::now();
    auto config = fixture.config(4);
    config.ttl = 5ms;
    ControlTrustedHostInventory inventory(config, [&] { return now; });
    std::mutex outcomes_mutex;
    std::vector<ControlTrustedHostInventoryStatus> outcomes;
    std::vector<std::thread> workers;
    for (int index = 0; index < 12; ++index) {
        workers.emplace_back([&] {
            const auto outcome = inventory.prepare(fixture.intent()).status;
            std::lock_guard lock(outcomes_mutex);
            outcomes.push_back(outcome);
        });
    }
    for (auto& worker : workers)
        worker.join();
    CHECK(std::count(outcomes.begin(), outcomes.end(),
                     ControlTrustedHostInventoryStatus::Prepared) == 4);
    CHECK(inventory.size() == 4);
    now += 6ms;
    for (const auto& outcome : outcomes)
        CHECK((outcome == ControlTrustedHostInventoryStatus::Prepared ||
               outcome == ControlTrustedHostInventoryStatus::ResourceExhausted));
    CHECK(inventory.sweep() == 4);
    CHECK(inventory.size() == 0);
    const auto expiring = inventory.prepare(fixture.intent());
    REQUIRE(expiring.ticket);
    now += 6ms;
    CHECK_FALSE(inventory.consume(expiring.ticket->inventory_id));
    CHECK(inventory.size() == 0);
#endif
}
