#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"
#include "control_static_code_identity.hpp"
#include "support/control_runtime_closure_sanitizer.hpp"

#include <pulp/inspect/control_client.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/inspect/control_trusted_host_inventory.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mach-o/dyld.h>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

class TemporaryRoot {
  public:
    TemporaryRoot() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random);
        path = std::filesystem::path{"/private/tmp"} /
               ("pulp-a3-product-" + pulp::runtime::hex_encode(*random));
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

class ScopedEnvironment {
  public:
    ScopedEnvironment(const char* name, std::optional<std::string> value) : name_(name) {
        if (const auto* current = std::getenv(name))
            previous_ = current;
        if (value)
            ::setenv(name, value->c_str(), 1);
        else
            ::unsetenv(name);
    }

    ~ScopedEnvironment() {
        if (previous_)
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

std::filesystem::path current_executable() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code error;
    const auto result = std::filesystem::weakly_canonical(buffer.data(), error);
    return error ? std::filesystem::path{} : result;
}

struct ProductResponse {
    std::string instance_id;
    std::string registration_id;
    std::string publication_id;
    std::string request_id;
    std::string request_hash;
    std::string receipt_id;
    std::string detail_json;
};

ProductResponse run_campaign(bool use_gpu) {
    TemporaryRoot root;
    const auto broker_executable = current_executable();
    const std::filesystem::path host{PULP_CONTROL_GPU_HEALTH_STANDALONE_PRODUCT_FIXTURE};
    REQUIRE_FALSE(broker_executable.empty());
    REQUIRE(std::filesystem::is_regular_file(host));

    ScopedEnvironment screenshot{"PULP_A3_PRODUCT_SCREENSHOT",
                                 (root.path / "product.png").string()};
    ScopedEnvironment disable_gpu{"PULP_A3_PRODUCT_DISABLE_GPU",
                                  use_gpu ? std::nullopt : std::optional<std::string>{"1"}};

    const ControlTrustedHostLaunchIntent host_intent{
        .executable = host,
        .arguments = {},
        .working_directory = host.parent_path(),
        .host_tier = ControlHostTier::Standalone,
    };
    REQUIRE(detail::inspect_static_code_identity(broker_executable));
    REQUIRE(pin_control_trusted_host_preparation_policy(host_intent));

    ControlBrokerDaemon daemon({
        .runtime_root = root.path / "runtime",
        .state_root = root.path / "state",
        .sdk_version = "0.820.0-a3-product-test",
        .executable_path = broker_executable,
        .process_generation = use_gpu ? 301U : 302U,
        .installed_host_selections = {{.host_id = use_gpu ? "a3-gpu-product" : "a3-cpu-negative",
                                       .intent = host_intent}},
        .decide_consent =
            [](const ControlGrantConsentRequest&) {
                return ControlConsentDecision{true,
                                              ControlConsentAuthority::ExistingUserPolicy,
                                              "a3-product-existing-policy",
                                              {}};
            },
    });
    REQUIRE(daemon.start());

    ControlClientConnection connection(
        {.endpoint_path = daemon.endpoint_path(), .expected_broker_executable = broker_executable});
    REQUIRE(connection.connect());
    const auto enrolled = connection.manage("enroll");
    INFO(enrolled.explanation);
    REQUIRE(enrolled.status_id == "accepted");
    const auto client_id =
        std::string(choc::json::parse(enrolled.data_json)["client_id"].getString());

    auto selection = choc::value::createObject("");
    selection.addMember("host_id",
                        choc::value::createString(use_gpu ? "a3-gpu-product" : "a3-cpu-negative"));
    const auto prepared =
        connection.manage("host-prepare-installed", choc::json::toString(selection, false), 15s);
    INFO(prepared.explanation);
    REQUIRE(prepared.status_id == "prepared");
    auto launch = choc::value::createObject("");
    launch.addMember("inventory_id",
                     choc::value::createString(std::string(
                         choc::json::parse(prepared.data_json)["inventory_id"].getString())));
    const auto launched =
        connection.manage("host-launch", choc::json::toString(launch, false), 15s);
    INFO(launched.explanation);
    REQUIRE(launched.status_id == "launched");

    ProductResponse product;
    const auto registration_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < registration_deadline) {
        const auto inventory = connection.manage("instances");
        REQUIRE(inventory.status_id == "completed");
        const auto inventory_data = choc::json::parse(inventory.data_json);
        const auto instances = inventory_data["instances"];
        if (instances.size() == 1) {
            product.instance_id = std::string(instances[0]["instance_id"].getString());
            product.registration_id = std::string(instances[0]["registration_id"].getString());
            product.publication_id = std::string(instances[0]["publication_id"].getString());
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE_FALSE(product.instance_id.empty());

    auto grant = choc::value::createObject("");
    grant.addMember("instance_id", choc::value::createString(product.instance_id));
    grant.addMember("operation_id", choc::value::createString("dev.pulp.gpu/health.read@1"));
    const auto granted = connection.manage("grant-request", choc::json::toString(grant, false));
    INFO(granted.explanation);
    REQUIRE(granted.status_id == "granted");
    const auto grant_id = std::string(choc::json::parse(granted.data_json)["grant_id"].getString());

    ControlClient client(connection);
    REQUIRE(client.negotiate({.mandatory_features = {"receipts"}}).succeeded());

    const auto response_deadline = std::chrono::steady_clock::now() + 10s;
    std::uint64_t sequence = 0;
    while (std::chrono::steady_clock::now() < response_deadline) {
        ++sequence;
        ControlRequestEnvelope request{
            .request_id = "a3-product-health-" + std::to_string(sequence),
            .client_id = client_id,
            .registration_id = product.registration_id,
            .grant_id = grant_id,
            .instance_generation = product.publication_id,
            .operation_id = "dev.pulp.gpu/health.read@1",
            .operation_version = 1,
            .idempotency_key = "a3-product-health-key-" + std::to_string(sequence),
            .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    (std::chrono::system_clock::now() + 5s).time_since_epoch())
                                    .count(),
            .params_json = "{}",
        };
        request.request_hash = *control_request_hash(request);
        const auto result = client.request(request, 5s);
        INFO(result.error_code);
        INFO(result.explanation);
        REQUIRE(result.succeeded());
        REQUIRE(result.response);
        REQUIRE(result.response->state == ControlReceiptState::Completed);
        const auto detail = choc::json::parse(result.response->detail_json);
        if (detail["startup"]["trials"].size() != 0) {
            product.request_id = request.request_id;
            product.request_hash = request.request_hash;
            product.receipt_id = result.response->receipt_id;
            product.detail_json = result.response->detail_json;
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE_FALSE(product.detail_json.empty());
    connection.disconnect();
    daemon.stop();
    return product;
}

bool authentic_capture(const choc::value::ValueView& detail) {
    const auto probe = detail["health"]["probes"][0];
    if (probe["adapter"]["status"].getString() != "authentic" ||
        probe["adapter"]["class"].getString() != "hardware" ||
        !probe["measurements"]["readback_completed"].getBool() ||
        !probe["measurements"]["content_floor_passed"].getBool() ||
        !probe["measurements"]["command_submitted"].isVoid())
        return false;
    for (const auto event : probe["events"])
        if (event["code"].getString() == "gpu.submit.unverified" &&
            event["verdict"].getString() == "unverified")
            return true;
    return false;
}

} // namespace

TEST_CASE("exact Standalone product instance publishes capture-only GPU health") {
    if (pulp::test::skip_when_sanitizer_perturbs_runtime_closure())
        return;
    const auto product = run_campaign(true);
    INFO(product.detail_json);
    const auto detail = choc::json::parse(product.detail_json);

    REQUIRE(detail["schema"].getString() == "pulp.gpu-health-read-result.v1");
    REQUIRE(detail["health"]["verdict"].getString() == "unverified");
    REQUIRE(detail["health"]["health_state"].getString() == "unverified");
    REQUIRE(authentic_capture(detail));

    const auto startup = detail["startup"];
    REQUIRE(startup["status"].getString() == "incomplete");
    REQUIRE(startup["verdict"].getString() == "unverified");
    REQUIRE(startup["budget"]["status"].getString() == "unratified");
    REQUIRE(startup["trials"].size() == 1);
    REQUIRE(startup["trials"][0]["editor_open_to_first_nonblank_ms"].isFloat());
    REQUIRE(startup["trials"][0]["content_floor_passed"].getBool());
    REQUIRE(startup["trials"][0]["verdict"].getString() == "unverified");
    REQUIRE(startup["correlation"]["gpu_evidence_id"].isVoid());
    REQUIRE(startup["correlation"]["trace_evidence_id"].isVoid());

    REQUIRE_FALSE(product.instance_id.empty());
    REQUIRE_FALSE(product.registration_id.empty());
    REQUIRE_FALSE(product.publication_id.empty());
    REQUIRE_FALSE(product.request_id.empty());
    REQUIRE_FALSE(product.request_hash.empty());
    REQUIRE_FALSE(product.receipt_id.empty());

    if (const auto* evidence_path = std::getenv("PULP_A3_EVIDENCE_RESPONSE_PATH")) {
        const auto* source_revision = std::getenv("PULP_A3_EVIDENCE_SOURCE_REVISION");
        REQUIRE(source_revision);
        const std::string_view source_revision_view{source_revision};
        REQUIRE(source_revision_view.size() == 40);
        REQUIRE(std::ranges::all_of(source_revision_view, [](unsigned char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        }));
        const std::string response_bytes = product.detail_json + '\n';
        const auto fixture_digest = pulp::runtime::sha256_file_hex(
            PULP_CONTROL_GPU_HEALTH_STANDALONE_PRODUCT_FIXTURE, 512U * 1024U * 1024U);
        REQUIRE(fixture_digest);

        std::ofstream output(evidence_path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << response_bytes;
        REQUIRE(output.good());

        auto binding = choc::value::createObject("");
        binding.addMember(
            "kind", choc::value::createString("pulp.gpu-first-visible-a3-product-observation"));
        binding.addMember("format_version", choc::value::createInt64(1));
        binding.addMember("status", choc::value::createString("nonterminal"));
        binding.addMember("source_revision", choc::value::createString(source_revision));
        binding.addMember("operation_id", choc::value::createString("dev.pulp.gpu/health.read@1"));
        binding.addMember("instance_id", choc::value::createString(product.instance_id));
        binding.addMember("registration_id", choc::value::createString(product.registration_id));
        binding.addMember("publication_id", choc::value::createString(product.publication_id));
        binding.addMember("request_id", choc::value::createString(product.request_id));
        binding.addMember("request_sha256", choc::value::createString(product.request_hash));
        binding.addMember("receipt_id", choc::value::createString(product.receipt_id));
        binding.addMember("response_sha256",
                          choc::value::createString(pulp::runtime::sha256_hex(response_bytes)));
        binding.addMember("fixture_sha256", choc::value::createString(*fixture_digest));
        binding.addMember("acceptance_disposition", choc::value::createString("withheld"));
        std::ofstream binding_output(std::string(evidence_path) + ".binding.json",
                                     std::ios::binary | std::ios::trunc);
        REQUIRE(binding_output.good());
        binding_output << choc::json::toString(binding, true) << '\n';
        REQUIRE(binding_output.good());
    }
}

TEST_CASE("CPU Standalone product cannot satisfy the authentic GPU capture gate") {
    if (pulp::test::skip_when_sanitizer_perturbs_runtime_closure())
        return;
    const auto product = run_campaign(false);
    REQUIRE_FALSE(authentic_capture(choc::json::parse(product.detail_json)));
}
