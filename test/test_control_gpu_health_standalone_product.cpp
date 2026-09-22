#include <catch2/catch_test_macros.hpp>

#include "control_broker_daemon.hpp"
#include "control_static_code_identity.hpp"
#include "support/a3_control_build_identity.hpp"
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

std::filesystem::path stage_product_host(const TemporaryRoot& root) {
    const std::filesystem::path source{PULP_CONTROL_GPU_HEALTH_STANDALONE_PRODUCT_FIXTURE};
    const std::filesystem::path source_manifest{source.string() +
                                                ".inspector-capabilities.json"};
    REQUIRE(std::filesystem::is_regular_file(source));
    REQUIRE(std::filesystem::is_regular_file(source_manifest));

    const auto directory = root.path / "host";
    REQUIRE(std::filesystem::create_directory(directory));
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);

    const auto executable = directory / source.filename();
    const std::filesystem::path manifest{executable.string() +
                                         ".inspector-capabilities.json"};
    REQUIRE(std::filesystem::copy_file(source, executable));
    REQUIRE(std::filesystem::copy_file(source_manifest, manifest));
    std::filesystem::permissions(executable,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    std::filesystem::permissions(manifest,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    return executable;
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

ControlRequestEnvelope make_trace_request(const std::string& client_id,
                                          const std::string& registration_id,
                                          const std::string& publication_id,
                                          const std::string& grant_id,
                                          const std::string& request_id, std::string params_json) {
    ControlRequestEnvelope request{
        .request_id = request_id,
        .client_id = client_id,
        .registration_id = registration_id,
        .grant_id = grant_id,
        .instance_generation = publication_id,
        .operation_id = "dev.pulp.trace/session-control@1",
        .operation_version = 1,
        .idempotency_key = "a3-product-trace-key-" + request_id,
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 10s).time_since_epoch())
                                .count(),
        .params_json = std::move(params_json),
    };
    request.request_hash = *control_request_hash(request);
    return request;
}

ControlRequestEnvelope
make_session_request(const std::string& client_id, const std::string& registration_id,
                     const std::string& publication_id, const std::string& grant_id,
                     const std::string& request_id, std::string params_json) {
    ControlRequestEnvelope request{
        .request_id = request_id,
        .client_id = client_id,
        .registration_id = registration_id,
        .grant_id = grant_id,
        .instance_generation = publication_id,
        .operation_id = "dev.pulp.session/control@1",
        .operation_version = 1,
        .idempotency_key = "a3-product-session-key-" + request_id,
        .deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                (std::chrono::system_clock::now() + 10s).time_since_epoch())
                                .count(),
        .params_json = std::move(params_json),
    };
    request.request_hash = *control_request_hash(request);
    return request;
}

ProductResponse run_campaign(bool use_gpu, bool seed_blank = false) {
    TemporaryRoot root;
    const auto broker_executable = current_executable();
    const auto host = stage_product_host(root);
    REQUIRE_FALSE(broker_executable.empty());

    ScopedEnvironment temporary_directory{"TMPDIR", root.path.string()};
    ScopedEnvironment screenshot{"PULP_A3_PRODUCT_SCREENSHOT",
                                 (root.path / "product.png").string()};
    ScopedEnvironment disable_gpu{"PULP_A3_PRODUCT_DISABLE_GPU",
                                  use_gpu ? std::nullopt : std::optional<std::string>{"1"}};
    ScopedEnvironment blank_first_frame{"PULP_GPU_HEALTH_SEED_BLANK_FRAME",
                                        seed_blank ? std::optional<std::string>{"1"}
                                                   : std::nullopt};

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

    std::optional<std::string> trace_grant_id;
    std::optional<std::string> controller_grant_id;
    std::optional<std::string> controller_lease_id;
    if (std::getenv("PULP_A3_CAPTURE_TRACE") && use_gpu && !seed_blank) {
        auto controller_grant = choc::value::createObject("");
        controller_grant.addMember("instance_id", choc::value::createString(product.instance_id));
        controller_grant.addMember("operation_id",
                                   choc::value::createString("dev.pulp.session/control@1"));
        const auto controller_granted =
            connection.manage("grant-request", choc::json::toString(controller_grant, false));
        INFO(controller_granted.explanation);
        REQUIRE(controller_granted.status_id == "granted");
        controller_grant_id =
            std::string(choc::json::parse(controller_granted.data_json)["grant_id"].getString());
        REQUIRE_FALSE(controller_grant_id->empty());

        const auto acquired = client.request(
            make_session_request(client_id, product.registration_id, product.publication_id,
                                 *controller_grant_id, "a3-product-session-acquire",
                                 R"({"action":"acquire"})"),
            10s);
        INFO(acquired.error_code);
        INFO(acquired.explanation);
        REQUIRE(acquired.succeeded());
        REQUIRE(acquired.response);
        REQUIRE(acquired.response->state == ControlReceiptState::Completed);
        REQUIRE_FALSE(acquired.response->result_code);
        const auto acquire_detail = choc::json::parse(acquired.response->detail_json);
        REQUIRE(acquire_detail["lease_id"].isString());
        controller_lease_id = std::string(acquire_detail["lease_id"].getString());
        REQUIRE_FALSE(controller_lease_id->empty());

        const auto renew_controller = [&](const char* request_id) {
            const auto renewed = client.request(
                make_session_request(client_id, product.registration_id, product.publication_id,
                                     *controller_grant_id, request_id, R"({"action":"renew"})"),
                10s);
            INFO(renewed.error_code);
            INFO(renewed.explanation);
            REQUIRE(renewed.succeeded());
            REQUIRE(renewed.response);
            REQUIRE(renewed.response->state == ControlReceiptState::Completed);
            REQUIRE_FALSE(renewed.response->result_code);
            const auto renew_detail = choc::json::parse(renewed.response->detail_json);
            REQUIRE(renew_detail["lease_id"].isString());
            REQUIRE(renew_detail["lease_id"].getString() == *controller_lease_id);
        };

        auto trace_grant = choc::value::createObject("");
        trace_grant.addMember("instance_id", choc::value::createString(product.instance_id));
        trace_grant.addMember("operation_id",
                              choc::value::createString("dev.pulp.trace/session-control@1"));
        const auto trace_granted =
            connection.manage("grant-request", choc::json::toString(trace_grant, false));
        INFO(trace_granted.explanation);
        REQUIRE(trace_granted.status_id == "granted");
        trace_grant_id =
            std::string(choc::json::parse(trace_granted.data_json)["grant_id"].getString());
        REQUIRE_FALSE(trace_grant_id->empty());

        const auto started = client.request(
            make_trace_request(
                client_id, product.registration_id, product.publication_id, *trace_grant_id,
                "a3-product-trace-start",
                R"({"action":"start","categories":["render","gpu","state","js","layout","text","canvas"],"ring_mb":128})"),
            10s);
        INFO(started.error_code);
        INFO(started.explanation);
        REQUIRE(started.succeeded());
        REQUIRE(started.response);
        REQUIRE(started.response->state == ControlReceiptState::Completed);
        REQUIRE_FALSE(started.response->result_code);

        // The health campaign is deliberately allowed to run for up to 10s;
        // renew immediately before it and again before stop so the controller
        // lease covers the entire trace window even on a slow GPU startup.
        renew_controller("a3-product-session-renew-before-health");
    }

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

    if (trace_grant_id) {
        const auto renewed = client.request(
            make_session_request(client_id, product.registration_id, product.publication_id,
                                 *controller_grant_id, "a3-product-session-renew-before-stop",
                                 R"({"action":"renew"})"),
            10s);
        INFO(renewed.error_code);
        INFO(renewed.explanation);
        REQUIRE(renewed.succeeded());
        REQUIRE(renewed.response);
        REQUIRE(renewed.response->state == ControlReceiptState::Completed);
        REQUIRE_FALSE(renewed.response->result_code);
        const auto renew_detail = choc::json::parse(renewed.response->detail_json);
        REQUIRE(renew_detail["lease_id"].isString());
        REQUIRE(renew_detail["lease_id"].getString() == *controller_lease_id);

        const auto stop_request =
            make_trace_request(client_id, product.registration_id, product.publication_id,
                               *trace_grant_id, "a3-product-trace-stop", R"({"action":"stop"})");
        auto stopped = client.request(stop_request, 15s);
        for (unsigned attempt = 0; attempt < 100 && stopped.response &&
                                   stopped.response->state != ControlReceiptState::Completed;
             ++attempt) {
            std::this_thread::sleep_for(20ms);
            stopped = client.request(stop_request, 15s);
        }
        INFO(stopped.error_code);
        INFO(stopped.explanation);
        if (stopped.response) {
            INFO(stopped.response->explanation);
            INFO(stopped.response->detail_json);
        }
        REQUIRE(stopped.succeeded());
        REQUIRE(stopped.response);
        REQUIRE(stopped.response->state == ControlReceiptState::Completed);
        const auto stop_detail = choc::json::parse(stopped.response->detail_json);
        REQUIRE(stop_detail["trace_bytes"].isInt64());
        REQUIRE(stop_detail["trace_bytes"].getInt64() > 0);
        const auto trace_path =
            std::filesystem::path{std::string(stop_detail["out_path"].getString())};
        REQUIRE_FALSE(std::filesystem::is_symlink(trace_path));
        REQUIRE(std::filesystem::is_regular_file(trace_path));
        std::error_code size_error;
        REQUIRE(std::filesystem::file_size(trace_path, size_error) > 0);
        REQUIRE_FALSE(size_error);
        std::error_code canonical_error;
        const auto canonical_root = std::filesystem::weakly_canonical(root.path, canonical_error);
        REQUIRE_FALSE(canonical_error);
        const auto canonical_trace = std::filesystem::weakly_canonical(trace_path, canonical_error);
        REQUIRE_FALSE(canonical_error);
        const auto root_prefix = canonical_root.generic_string() + "/";
        REQUIRE(canonical_trace.generic_string().starts_with(root_prefix));
        if (const auto* destination = std::getenv("PULP_A3_TRACE_CAPTURE_PATH")) {
            const auto target = std::filesystem::path{destination};
            std::error_code copy_error;
            std::filesystem::copy_file(
                trace_path, target, std::filesystem::copy_options::overwrite_existing, copy_error);
            REQUIRE_FALSE(copy_error);
        }

        const auto released = client.request(
            make_session_request(client_id, product.registration_id, product.publication_id,
                                 *controller_grant_id, "a3-product-session-release",
                                 R"({"action":"release"})"),
            10s);
        INFO(released.error_code);
        INFO(released.explanation);
        REQUIRE(released.succeeded());
        REQUIRE(released.response);
        REQUIRE(released.response->state == ControlReceiptState::Completed);
        REQUIRE_FALSE(released.response->result_code);
    }
    connection.disconnect();
    daemon.stop();
    return product;
}

// True when the published probe reports a host that observed the captured
// frame's submission. A host without a submission producer leaves the
// measurement absent rather than false, so both shapes are read here.
bool submission_observed(const choc::value::ValueView& detail) {
    const auto submitted = detail["health"]["probes"][0]["measurements"]["command_submitted"];
    return submitted.isBool() && submitted.getBool();
}

// The capture half of the probe is host-independent: an authentic hardware
// adapter, a completed readback, and content above the floor. The submit
// stage is not -- it follows the window host's submission evidence, so the
// caller says which stage it expects and this checks the matching event.
bool authentic_capture(const choc::value::ValueView& detail, bool submitted) {
    const auto probe = detail["health"]["probes"][0];
    if (probe["adapter"]["status"].getString() != "authentic" ||
        probe["adapter"]["class"].getString() != "hardware" ||
        !probe["measurements"]["readback_completed"].getBool() ||
        !probe["measurements"]["content_floor_passed"].getBool())
        return false;
    const std::string_view expected_code = submitted ? "gpu.submit.pass" : "gpu.submit.unverified";
    const std::string_view expected_verdict = submitted ? "pass" : "unverified";
    for (const auto event : probe["events"])
        if (event["code"].getString() == expected_code &&
            event["verdict"].getString() == expected_verdict)
            return true;
    return false;
}

} // namespace

TEST_CASE("exact Standalone product instance publishes host-evidenced GPU health") {
    if (pulp::test::skip_when_sanitizer_perturbs_runtime_closure())
        return;
    const auto product = run_campaign(true);
    INFO(product.detail_json);
    const auto detail = choc::json::parse(product.detail_json);

    REQUIRE(detail["schema"].getString() == "pulp.gpu-health-read-result.v1");
    // A host that observes the captured frame's submission publishes a
    // passing submit stage; one that does not leaves it unverified. Which
    // one a given host yields is a property of the host, so the expected
    // aggregate is derived from the measurement rather than pinned.
    const bool submitted = submission_observed(detail);
    REQUIRE(detail["health"]["verdict"].getString() == (submitted ? "pass" : "unverified"));
    REQUIRE(detail["health"]["health_state"].getString() == (submitted ? "healthy" : "unverified"));
    REQUIRE(authentic_capture(detail, submitted));

    const auto startup = detail["startup"];
    REQUIRE(startup["status"].getString() == "incomplete");
    REQUIRE(startup["verdict"].getString() == "unverified");
    REQUIRE(startup["budget"]["status"].getString() == "unratified");
    REQUIRE(startup["trials"].size() == 1);
    REQUIRE(startup["trials"][0]["editor_open_to_first_nonblank_ms"].isFloat());
    REQUIRE(startup["trials"][0]["content_floor_passed"].getBool());
    REQUIRE(startup["trials"][0]["verdict"].getString() == "unverified");
    const auto gpu_evidence_id = std::string(startup["correlation"]["gpu_evidence_id"].getString());
    const auto trace_evidence_id =
        std::string(startup["correlation"]["trace_evidence_id"].getString());
    REQUIRE(gpu_evidence_id.size() == 32);
    REQUIRE(std::ranges::all_of(gpu_evidence_id, [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    }));
    REQUIRE(trace_evidence_id.starts_with("trace-"));
    REQUIRE(trace_evidence_id != gpu_evidence_id);

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
    const auto detail = choc::json::parse(product.detail_json);
    // A CPU product fails the capture half outright -- no authentic hardware
    // adapter -- so neither submit-stage expectation can rescue it.
    REQUIRE_FALSE(authentic_capture(detail, /*submitted=*/false));
    REQUIRE_FALSE(authentic_capture(detail, /*submitted=*/true));
}

TEST_CASE("exact Standalone product catches the seeded transparent first frame") {
    if (pulp::test::skip_when_sanitizer_perturbs_runtime_closure())
        return;
    const auto product = run_campaign(true, true);
    INFO(product.detail_json);
    const auto detail = choc::json::parse(product.detail_json);
    REQUIRE(detail["health"]["verdict"].getString() == "fail");
    REQUIRE(detail["startup"]["trials"].size() == 1);
    REQUIRE(detail["startup"]["trials"][0]["diagnostic_code"].getString() == "gpu.startup.blank");
    REQUIRE_FALSE(detail["startup"]["trials"][0]["content_floor_passed"].getBool());

    const auto* receipt_path = std::getenv("PULP_A3_BLANK_NEGATIVE_RECEIPT_PATH");
    if (!receipt_path || receipt_path[0] == '\0')
        return;
    auto receipt = choc::value::createObject("");
    receipt.addMember("schema",
                      choc::value::createString("pulp.gpu-first-visible-blank-negative.v1"));
    receipt.addMember("version", choc::value::createInt64(1));
    receipt.addMember("injection", choc::value::createString("transparent-first-frame"));
    receipt.addMember("expected_diagnostic_code", choc::value::createString("gpu.startup.blank"));
    receipt.addMember("observed_diagnostic_code", choc::value::createString("gpu.startup.blank"));
    receipt.addMember("caught", choc::value::createBool(true));
    receipt.addMember(
        "control_build",
        choc::json::parse(std::string(pulp::test::kA3ControlBuildIdentityJson)));
    std::ofstream output(receipt_path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << choc::json::toString(receipt, true) << '\n';
    REQUIRE(output.good());
}
