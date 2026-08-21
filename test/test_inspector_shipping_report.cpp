#include "tools/cli/inspector_shipping_report.hpp"
#include "reload_test_support.hpp"

#include <pulp/inspect/control_manifest.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

namespace fs = std::filesystem;

namespace {
struct TemporaryDirectory {
    fs::path path = pulp::test::unique_tmp_dir("pulp-inspector-shipping-report-");
    TemporaryDirectory() { fs::create_directories(path / "pulp-inspector-manifests"); }
    ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};

struct ScopedCurrentDirectory {
    fs::path original = fs::current_path();
    explicit ScopedCurrentDirectory(const fs::path& path) {
        fs::current_path(path);
    }
    ~ScopedCurrentDirectory() {
        std::error_code error;
        fs::current_path(original, error);
    }
};

void write_manifest(const fs::path& path, std::string_view body) {
    std::ofstream output(path);
    output << body;
}

void write_artifact(const fs::path& path, std::string_view body) {
    std::ofstream output(path, std::ios::binary);
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    output.close();
    fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
}

std::string standalone_artifact(std::string_view suffix = {}) {
    std::string result = "PULP_STANDALONE_";
    result += "COMPONENT_V1";
    result += suffix;
    return result;
}
} // namespace

TEST_CASE("inspector shipping report preserves ordinary and declared targets") {
    TemporaryDirectory temporary;
    const auto manifests = temporary.path / "pulp-inspector-manifests";
    write_manifest(manifests / "ordinary.json",
        R"({"product_name": "Ordinary","shipping_override": false,"unsafe_runtime_eval_acknowledged": false,"capabilities":[]})");
    write_manifest(manifests / "developer.json",
        R"({"product_name": "Developer","shipping_override": true,"unsafe_runtime_eval_acknowledged": false,"capabilities":["ui.read"]})");

    const auto report =
        pulp::cli::inspector_shipping::load_report(temporary.path);
    REQUIRE(report.complete);
    CHECK(report.ships_inspector);
    CHECK_FALSE(report.ships_runtime_eval);
    CHECK(report.json.find("\"ui.read\"") != std::string::npos);
}

TEST_CASE("runtime evaluation acknowledgement remains independently visible") {
    TemporaryDirectory temporary;
    write_manifest(temporary.path / "pulp-inspector-manifests" / "unsafe.json",
        R"({"product_name": "Unsafe","shipping_override": true,"unsafe_runtime_eval_acknowledged": true,"capabilities":["runtime.eval"]})");
    const auto report =
        pulp::cli::inspector_shipping::load_report(temporary.path);
    REQUIRE(report.complete);
    REQUIRE(report.ships_inspector);
    REQUIRE(report.ships_runtime_eval);

    const auto evidence = temporary.path / "evidence.json";
    REQUIRE(pulp::cli::inspector_shipping::write_evidence(
        evidence, report, "package-input"));
    std::ifstream input(evidence);
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    CHECK(json.find("\"operation\": \"package-input\"") != std::string::npos);
    CHECK(json.find("\"runtime.eval\"") != std::string::npos);
}

TEST_CASE("inspector shipping report fails closed and scopes one product") {
    TemporaryDirectory temporary;
    const auto manifests = temporary.path / "pulp-inspector-manifests";

    const auto missing = pulp::cli::inspector_shipping::load_report(temporary.path);
    CHECK_FALSE(missing.complete);
    CHECK_FALSE(missing.error.empty());

    write_manifest(manifests / "ordinary.json",
        R"({"product_name": "Ordinary","shipping_override": false,"unsafe_runtime_eval_acknowledged": false,"capabilities":[]})");
    write_manifest(manifests / "developer.json",
        R"({"product_name": "Developer","shipping_override": true,"unsafe_runtime_eval_acknowledged": false,"capabilities":["ui.read"]})");

    const auto ordinary = pulp::cli::inspector_shipping::load_report(
        temporary.path, "Ordinary");
    REQUIRE(ordinary.complete);
    CHECK_FALSE(ordinary.ships_inspector);
    CHECK(ordinary.json.find("Developer") == std::string::npos);

    const auto unknown = pulp::cli::inspector_shipping::load_report(
        temporary.path, "Missing");
    CHECK_FALSE(unknown.complete);
    CHECK_FALSE(unknown.error.empty());
}

TEST_CASE("artifact scan rejects a stale manifest before packaging") {
    TemporaryDirectory temporary;
    const auto manifest_path =
        temporary.path / "pulp-inspector-manifests" / "ordinary.json";
    write_manifest(manifest_path,
        R"({"product_name":"Ordinary","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    const auto report = pulp::cli::inspector_shipping::load_report(temporary.path);
    REQUIRE(report.complete);
    REQUIRE(report.manifests.size() == 1);

    const auto artifact = temporary.path / "Ordinary";
    write_artifact(artifact, standalone_artifact(" ordinary-product"));
    std::string error;
    CHECK(pulp::cli::inspector_shipping::scan_artifact(
        artifact, report.manifests.front(), error));

    write_artifact(artifact,
        standalone_artifact(" PULP_INSPECT_SHIPPING_MANIFEST_V1"));
    error.clear();
    CHECK_FALSE(pulp::cli::inspector_shipping::scan_artifact(
        artifact, report.manifests.front(), error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("artifact scan accepts exact declared capability markers") {
    TemporaryDirectory temporary;
    const auto manifest_path =
        temporary.path / "pulp-inspector-manifests" / "developer.json";
    write_manifest(manifest_path,
        R"({"product_name":"Developer","shipping_override":true,"unsafe_runtime_eval_acknowledged":false,"capabilities":["ui.read"]})");
    const auto report = pulp::cli::inspector_shipping::load_report(temporary.path);
    REQUIRE(report.complete);
    REQUIRE(report.manifests.size() == 1);

    const auto artifact = temporary.path / "Developer";
    write_artifact(artifact, standalone_artifact(
        " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
        "PULP_INSPECT_CAPABILITY_UI_READ_V1"));
    std::string error;
    CHECK(pulp::cli::inspector_shipping::scan_artifact(
        artifact, report.manifests.front(), error));

}

TEST_CASE("canonical control manifest drives the legacy shipping scanner") {
    TemporaryDirectory temporary;
    pulp::inspect::ControlManifest control;
    control.profile = pulp::inspect::ControlBuildProfile::DeveloperLocal;
    control.target = "Canonical";
    control.product_name = "Canonical";
    control.bundle_id = "dev.pulp.test.canonical";
    control.build_id = "build:33333333333333333333333333333333";
    control.endpoint_included = true;
    control.capabilities = {
        pulp::inspect::InspectorCapability::UiRead,
        pulp::inspect::InspectorCapability::SessionControl,
        pulp::inspect::InspectorCapability::StateWrite,
    };
    const auto json = pulp::inspect::serialize_control_manifest(control);
    REQUIRE_FALSE(json.empty());
    write_manifest(
        temporary.path / "pulp-inspector-manifests" / "canonical.json", json);

    const auto report = pulp::cli::inspector_shipping::load_report(temporary.path);
    REQUIRE(report.complete);
    REQUIRE(report.manifests.size() == 1);
    CHECK(report.manifests.front().control_profile == "developer-local");
    CHECK(report.manifests.front().capabilities.size() == 3);
    CHECK(report.manifests.front().control_capabilities.size() == 3);

    const auto artifact = temporary.path / "Canonical";
    const auto digest = pulp::inspect::control_manifest_digest(control);
    write_artifact(
        artifact,
        standalone_artifact(
            " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
            "PULP_INSPECT_CAPABILITY_UI_READ_V1 "
            "PULP_INSPECT_CAPABILITY_SESSION_CONTROL_V1 "
            "PULP_INSPECT_CAPABILITY_STATE_WRITE_V1 "
            "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1 "
            "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1"));
    write_manifest(temporary.path / "Canonical.inspector-capabilities.json", json);
    write_manifest(temporary.path / "Unrelated.inspector-capabilities.json",
                   "not-json");
    std::string error;
    CHECK(pulp::cli::inspector_shipping::scan_artifact(
        artifact, report.manifests.front(), error));

    const auto audit = pulp::cli::inspector_shipping::audit_artifact(artifact);
    REQUIRE(audit.complete);
    const auto structured = pulp::cli::inspector_shipping::audit_json(audit);
    CHECK(structured.find("\"verdict\"") != std::string::npos);
    CHECK(structured.find("\"pass\"") != std::string::npos);
    CHECK(structured.find("dev.pulp.ui/observe@1") != std::string::npos);
    CHECK(structured.find("\"risk\": \"mutating\"") != std::string::npos);
    CHECK(structured.find("explicit client grant") != std::string::npos);
    CHECK(structured.find("\"artifactDigest\"") != std::string::npos);
    CHECK(structured.find("\"consentIdentity\"") != std::string::npos);
    const auto original_consent = audit.manifests.front().consent_identity;
    write_artifact(
        artifact,
        standalone_artifact(
            " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
            "PULP_INSPECT_CAPABILITY_UI_READ_V1 "
            "PULP_INSPECT_CAPABILITY_SESSION_CONTROL_V1 "
            "PULP_INSPECT_CAPABILITY_STATE_WRITE_V1 "
            "PULP_CONTROL_PROFILE_DEVELOPER_LOCAL_V1 "
            "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1 changed-byte"));
    const auto changed_audit =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    REQUIRE(changed_audit.complete);
    CHECK(changed_audit.manifests.front().consent_identity != original_consent);
    const auto human = pulp::cli::inspector_shipping::audit_human(audit);
    CHECK(human.find("PASS control artifact audit") != std::string::npos);
    CHECK(human.find("risk=mutating") != std::string::npos);
    CHECK(human.find("runtime mutation still requires") != std::string::npos);
}

TEST_CASE("production control audit blocks Remote View and reports OSC") {
    TemporaryDirectory temporary;
    pulp::inspect::ControlManifest control;
    control.target = "Production";
    control.product_name = "Production";
    control.bundle_id = "dev.pulp.test.production";
    control.build_id = "build:44444444444444444444444444444444";
    const auto json = pulp::inspect::serialize_control_manifest(control);
    const auto digest = pulp::inspect::control_manifest_digest(control);
    write_manifest(temporary.path / "Production.inspector-capabilities.json", json);

    const auto artifact = temporary.path / "Production";
    const auto base = standalone_artifact(
        " PULP_CONTROL_PROFILE_PRODUCTION_STRIPPED_V1 "
        "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1");
    write_artifact(artifact, base + " legacy-handler=view.param_set");
    auto blocked = pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(blocked.complete);
    CHECK(blocked.error.find("Remote View") != std::string::npos);
    REQUIRE(blocked.manifests.size() == 1);
    CHECK(blocked.manifests.front().remote_view_authority);
    CHECK(pulp::cli::inspector_shipping::audit_json(blocked).find(
              "remote-view-parameter-authority") != std::string::npos);

    write_artifact(artifact, base + " PULP_OSC_UDP_NETWORK_SURFACE_V1");
    const auto advisory =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    REQUIRE(advisory.complete);
    CHECK(advisory.manifests.front().osc_udp_network_surface);
    CHECK(pulp::cli::inspector_shipping::audit_json(advisory).find(
              "outside Product A policy") != std::string::npos);
}

TEST_CASE("control audit preserves stable manifest failure codes") {
    TemporaryDirectory temporary;
    pulp::inspect::ControlManifest control;
    control.target = "InvalidRegistry";
    control.product_name = "InvalidRegistry";
    control.build_id = "build:66666666666666666666666666666666";
    auto json = pulp::inspect::serialize_control_manifest(control);
    REQUIRE_FALSE(json.empty());
    const auto digest_position = json.find(pulp::inspect::kControlRegistryDigest);
    REQUIRE(digest_position != std::string::npos);
    json.replace(digest_position, 64, std::string(64, '0'));
    const auto artifact = temporary.path / "InvalidRegistry";
    write_artifact(artifact, standalone_artifact(""));
    write_manifest(
        temporary.path / "InvalidRegistry.inspector-capabilities.json", json);

    const auto directory_report =
        pulp::cli::inspector_shipping::audit_artifact(temporary.path);
    CHECK_FALSE(directory_report.complete);
    CHECK(directory_report.error_code == "manifest.registry-mismatch");

    const auto report =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(report.complete);
    CHECK(report.error_code == "manifest.registry-mismatch");
    CHECK(pulp::cli::inspector_shipping::audit_json(report).find(
              "manifest.registry-mismatch") != std::string::npos);
}

TEST_CASE("control audit never passes legacy identity-less evidence") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto artifact = temporary.path / "Legacy";
    write_artifact(artifact, standalone_artifact(" ordinary standalone"));
    write_manifest(temporary.path / "Legacy.inspector-capabilities.json",
        R"({"target":"Legacy","product_name":"Legacy","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");

    const auto direct =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(direct.complete);
    CHECK(direct.error_code == "audit.canonical-manifest-required");
    const auto directory =
        pulp::cli::inspector_shipping::audit_artifact(temporary.path);
    CHECK_FALSE(directory.complete);
    CHECK(directory.error_code == "audit.canonical-manifest-required");
}

TEST_CASE("schema-bearing manifests never fall through to legacy parsing") {
    TemporaryDirectory temporary;
    pulp::inspect::ControlManifest control;
    control.target = "FutureSchema";
    control.product_name = "FutureSchema";
    control.build_id = "build:77777777777777777777777777777777";
    auto json = pulp::inspect::serialize_control_manifest(control);
    const auto schema = json.find("artifact-manifest@1");
    REQUIRE(schema != std::string::npos);
    json.replace(schema, std::string("artifact-manifest@1").size(),
                 "artifact-manifest@2");
    write_manifest(temporary.path / "pulp-inspector-manifests" /
                       "FutureSchema.json",
                   json);

    const auto report =
        pulp::cli::inspector_shipping::load_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error_code == "manifest.unsupported-schema");

    write_manifest(temporary.path / "pulp-inspector-manifests" /
                       "FutureSchema.json",
        R"({"schema":2,"product_name":"LegacyShape","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    const auto non_string =
        pulp::cli::inspector_shipping::load_report(temporary.path);
    CHECK_FALSE(non_string.complete);
    CHECK(non_string.error_code == "manifest.unknown-field");

    write_manifest(temporary.path / "pulp-inspector-manifests" /
                       "FutureSchema.json",
        R"({"schema":null,"product_name":"LegacyShape","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    const auto null_schema =
        pulp::cli::inspector_shipping::load_report(temporary.path);
    CHECK_FALSE(null_schema.complete);
    CHECK(null_schema.error_code == "manifest.unknown-field");
}

TEST_CASE("direct control audit types malformed sidecar failures") {
    TemporaryDirectory temporary;
    const auto artifact = temporary.path / "Malformed";
    write_artifact(artifact, standalone_artifact(" ordinary standalone"));
    write_manifest(temporary.path / "Malformed.inspector-capabilities.json",
                   "not-json");

    const auto report =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(report.complete);
    CHECK(report.error_code == "audit.manifest-invalid");
    CHECK(pulp::cli::inspector_shipping::audit_json(report).find(
              "audit.manifest-invalid") != std::string::npos);

    write_manifest(temporary.path / "Malformed.inspector-capabilities.json",
                   std::string((1024 * 1024) + 1, 'x'));
    const auto oversized =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(oversized.complete);
    CHECK(oversized.error_code == "audit.manifest-invalid");
    CHECK(oversized.error.find("exceeds 1 MiB") != std::string::npos);
}

TEST_CASE("direct control audit binds exact sidecars to safe artifact identities") {
    TemporaryDirectory temporary;
    pulp::inspect::ControlManifest control;
    control.target = "Other";
    control.product_name = "Other";
    control.build_id = "build:99999999999999999999999999999999";
    auto json = pulp::inspect::serialize_control_manifest(control);
    REQUIRE_FALSE(json.empty());

    const auto artifact = temporary.path / "Direct";
    write_artifact(artifact, standalone_artifact(" ordinary standalone"));
    const auto sidecar =
        temporary.path / "Direct.inspector-capabilities.json";
    write_manifest(sidecar, json);

    const auto mismatch =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(mismatch.complete);
    CHECK(mismatch.error_code == "audit.manifest-invalid");
    CHECK(mismatch.error.find("must match its sidecar filename") !=
          std::string::npos);

    const auto target = json.find("\"target\": \"Other\"");
    REQUIRE(target != std::string::npos);
    json.replace(target, std::string("\"target\": \"Other\"").size(),
                 "\"target\": \"../Direct\"");
    write_manifest(sidecar, json);

    const auto unsafe =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(unsafe.complete);
    CHECK(unsafe.error_code == "audit.manifest-invalid");
    CHECK(unsafe.error.find("safe filename") != std::string::npos);
}

TEST_CASE("direct control audit rejects renamed canonical sidecars") {
    TemporaryDirectory temporary;
    pulp::inspect::ControlManifest control;
    control.target = "Target";
    control.product_name = "Product";
    control.build_id = "build:12121212121212121212121212121212";
    const auto json = pulp::inspect::serialize_control_manifest(control);
    const auto digest = pulp::inspect::control_manifest_digest(control);
    const auto artifact = temporary.path / "Product";
    write_artifact(artifact, standalone_artifact(
        " PULP_CONTROL_PROFILE_PRODUCTION_STRIPPED_V1 "
        "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1"));
    write_manifest(temporary.path / "Product.inspector-capabilities.json", json);

    const auto report =
        pulp::cli::inspector_shipping::audit_artifact(artifact);
    CHECK_FALSE(report.complete);
    CHECK(report.error_code == "audit.manifest-invalid");
    CHECK(report.error.find("must match its sidecar filename") !=
          std::string::npos);
}

TEST_CASE("ordinary artifacts may contain unrelated inspector-like names") {
    TemporaryDirectory temporary;
    const auto manifest_path =
        temporary.path / "pulp-inspector-manifests" / "ordinary.json";
    write_manifest(manifest_path,
        R"({"product_name":"Ordinary","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    const auto report = pulp::cli::inspector_shipping::load_report(temporary.path);
    REQUIRE(report.complete);
    REQUIRE(report.manifests.size() == 1);

    const auto artifact = temporary.path / "Ordinary";
    write_artifact(artifact, standalone_artifact(
        " third-party InspectorServer DiscoveryPublisher "
        "publish_discovery_record"));
    std::string error;
    CHECK(pulp::cli::inspector_shipping::scan_artifact(
        artifact, report.manifests.front(), error));
}

TEST_CASE("inspector evidence rejects capability aliases and stale build artifacts") {
    TemporaryDirectory temporary;
    const auto artifacts = temporary.path / "products" / "Alias.app" / "Contents/MacOS";
    fs::create_directories(artifacts);
    const auto sidecar = artifacts / "Alias.inspector-capabilities.json";
    write_manifest(sidecar,
        R"({"product_name":"Alias","shipping_override":true,"unsafe_runtime_eval_acknowledged":false,"capabilities":["state-write"]})");
    write_artifact(artifacts / "Alias", standalone_artifact(
        " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
        "PULP_INSPECT_CAPABILITY_STATE_WRITE_V1"));
    auto alias =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(alias.complete);
    CHECK(alias.error.find("unknown inspector capability") != std::string::npos);

    write_manifest(sidecar,
        R"({"product_name":"Alias","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    auto stale =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(stale.complete);
    CHECK(stale.error.find("does not match manifest") != std::string::npos);

    write_artifact(artifacts / "Alias",
        standalone_artifact(" ordinary standalone"));
    auto exact =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    REQUIRE(exact.complete);
    REQUIRE(exact.manifests.size() == 1);
    CHECK(exact.manifests.front().product_name == "Alias");
}

TEST_CASE("inspector evidence rejects standalone artifacts without copied sidecars") {
    TemporaryDirectory temporary;
    write_manifest(temporary.path / "pulp-inspector-manifests" / "Missing.json",
        R"({"target":"Missing","product_name":"Missing Product","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    const auto executable =
        temporary.path / "products" / "Missing Product.app" / "Contents/MacOS" /
        "Missing Product";
    fs::create_directories(executable.parent_path());
    write_artifact(executable, standalone_artifact(" ordinary standalone"));

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error.find("missing inspector capability sidecar") !=
          std::string::npos);

    fs::remove(temporary.path / "pulp-inspector-manifests" / "Missing.json");
    write_artifact(executable, standalone_artifact(
        " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
        "PULP_INSPECT_CAPABILITY_UI_READ_V1"));
    const auto unconfigured =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(unconfigured.complete);
    CHECK(unconfigured.error.find("missing inspector capability sidecar") !=
          std::string::npos);
}

TEST_CASE("container evidence requires sidecars for ordinary Pulp standalones") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto executable = temporary.path / "Ordinary.app" / "Contents" /
        "MacOS" / "custom-entry-point";
    fs::create_directories(executable.parent_path());
    write_artifact(executable, standalone_artifact(" ordinary standalone"));

    const auto missing =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    REQUIRE_FALSE(missing.complete);
    CHECK(missing.error.find("missing inspector capability sidecar") !=
          std::string::npos);

    write_manifest(executable.parent_path() /
                       "OrdinaryTarget.inspector-capabilities.json",
        R"({"target":"OrdinaryTarget","product_name":"Ordinary Product","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    write_artifact(executable.parent_path() / "libpulp-standalone.a",
        standalone_artifact());
    const auto complete =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    REQUIRE(complete.complete);
    REQUIRE(complete.manifests.size() == 1);
    CHECK(complete.manifests.front().product_name == "Ordinary Product");
}

TEST_CASE("mixed build trees reject unconfigured standalone artifacts") {
    TemporaryDirectory temporary;
    write_manifest(temporary.path / "pulp-inspector-manifests" / "Current.json",
        R"({"target":"Current","product_name":"Current","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    const auto current_dir =
        temporary.path / "products" / "Current.app" / "Contents/MacOS";
    fs::create_directories(current_dir);
    write_artifact(current_dir / "Current",
        standalone_artifact(" ordinary current standalone"));
    write_manifest(current_dir / "Current.inspector-capabilities.json",
        R"({"target":"Current","product_name":"Current","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");

    const auto stale_dir =
        temporary.path / "products" / "Removed.app" / "Contents/MacOS";
    fs::create_directories(stale_dir);
    write_artifact(stale_dir / "Removed", standalone_artifact(
        " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
        "PULP_INSPECT_CAPABILITY_UI_READ_V1"));

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error.find("Removed") != std::string::npos);
    CHECK(report.error.find("missing inspector capability sidecar") !=
          std::string::npos);
}

TEST_CASE("one sidecar cannot mask a sibling standalone executable") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto products = temporary.path / "products";
    fs::create_directories(products);
    write_artifact(products / "First.exe",
        standalone_artifact(" ordinary first standalone"));
    write_manifest(products / "First.inspector-capabilities.json",
        R"({"target":"First","product_name":"First","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    write_artifact(products / "Second.exe", standalone_artifact(
        " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
        "PULP_INSPECT_CAPABILITY_UI_READ_V1"));

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error.find("Second.exe") != std::string::npos);
    CHECK(report.error.find("missing inspector capability sidecar") !=
          std::string::npos);
}

TEST_CASE("target and product aliases cannot bind one sidecar twice") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto products = temporary.path / "products";
    fs::create_directories(products);
    write_artifact(products / "Target.exe",
        standalone_artifact(" ordinary target executable"));
    write_artifact(products / "Product.exe",
        standalone_artifact(" ordinary product executable"));
    write_manifest(products / "Target.inspector-capabilities.json",
        R"({"target":"Target","product_name":"Product","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error.find("multiple standalone executables") !=
          std::string::npos);
}

TEST_CASE("artifact sidecar identities cannot escape their directory") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto products = temporary.path / "products";
    fs::create_directories(products);
    write_artifact(products / "Escape",
        standalone_artifact(" ordinary standalone"));
    write_manifest(products / "Escape.inspector-capabilities.json",
        R"({"target":"Escape","product_name":"../Outside","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error_code == "audit.manifest-invalid");
    CHECK(report.error.find("safe filename") != std::string::npos);
}

TEST_CASE("canonical directory evidence cannot bind stale artifact identities") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    pulp::inspect::ControlManifest control;
    control.target = "Other";
    control.product_name = "Other";
    control.build_id = "build:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto json = pulp::inspect::serialize_control_manifest(control);
    const auto digest = pulp::inspect::control_manifest_digest(control);
    const auto binary = standalone_artifact(
        " PULP_CONTROL_PROFILE_PRODUCTION_STRIPPED_V1 "
        "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1");

    SECTION("sidecar stem must match canonical target") {
        write_manifest(
            temporary.path / "Alias.inspector-capabilities.json", json);
        write_artifact(temporary.path / "Alias", binary);
        const auto report =
            pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
        CHECK_FALSE(report.complete);
        CHECK(report.error_code == "audit.manifest-invalid");
        CHECK(report.error.find("must match its sidecar filename") !=
              std::string::npos);
    }

    SECTION("unique marker-bearing siblings do not replace canonical identity") {
        write_manifest(
            temporary.path / "Other.inspector-capabilities.json", json);
        write_artifact(temporary.path / "Renamed", binary);
        const auto report =
            pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
        CHECK_FALSE(report.complete);
        CHECK(report.error.find("could not resolve standalone executable") !=
              std::string::npos);
    }
}

TEST_CASE("AUv3 container apps are not standalone inspector artifacts") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto executable = temporary.path / "AUv3" / "PluginHost.app" /
        "Contents" / "MacOS" / "PluginHost";
    fs::create_directories(executable.parent_path());
    write_artifact(executable, "ordinary AUv3 host app");

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK(report.complete);
    CHECK(report.manifests.empty());
}

TEST_CASE("plugin-format sidecars never become standalone audit evidence") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    pulp::inspect::ControlManifest control;
    control.target = "Plugin";
    control.product_name = "Plugin";
    control.build_id = "build:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const auto json = pulp::inspect::serialize_control_manifest(control);
    const auto digest = pulp::inspect::control_manifest_digest(control);
    const auto executable = temporary.path / "Plugin.vst3" / "Contents" /
        "MacOS" / "Plugin";
    fs::create_directories(executable.parent_path());
    write_artifact(executable, standalone_artifact(
        " PULP_CONTROL_PROFILE_PRODUCTION_STRIPPED_V1 "
        "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1"));
    write_manifest(executable.parent_path() /
                       "Plugin.inspector-capabilities.json",
                   json);

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    REQUIRE(report.complete);
    CHECK(report.manifests.empty());
    const auto audit = pulp::cli::inspector_shipping::audit_artifact(
        temporary.path / "Plugin.vst3");
    CHECK_FALSE(audit.complete);
    CHECK(audit.error_code == "audit.no-artifact");
    const auto direct =
        pulp::cli::inspector_shipping::audit_artifact(executable);
    CHECK_FALSE(direct.complete);
    CHECK(direct.error_code == "audit.no-artifact");

    {
        ScopedCurrentDirectory cwd(executable.parent_path());
        const auto relative =
            pulp::cli::inspector_shipping::audit_artifact("Plugin");
        CHECK_FALSE(relative.complete);
        CHECK(relative.error_code == "audit.no-artifact");
    }

    const auto alias = temporary.path / "plugin-alias";
    fs::create_directory_symlink(executable.parent_path(), alias);
    const auto aliased =
        pulp::cli::inspector_shipping::audit_artifact(alias / "Plugin");
    CHECK_FALSE(aliased.complete);
    CHECK(aliased.error_code == "audit.no-artifact");
    fs::remove(alias);

    fs::remove(executable.parent_path() /
               "Plugin.inspector-capabilities.json");
    const auto outside_manifest = temporary.path / "outside.json";
    write_manifest(outside_manifest, json);
    fs::create_symlink(outside_manifest, executable.parent_path() /
                       "Plugin.inspector-capabilities.json");
    const auto symlink_ignored =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    REQUIRE(symlink_ignored.complete);
    CHECK(symlink_ignored.manifests.empty());
}

TEST_CASE("control audit rejects empty and unauditable plugin directories") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");

    const auto empty =
        pulp::cli::inspector_shipping::audit_artifact(temporary.path);
    REQUIRE_FALSE(empty.complete);
    CHECK(empty.error_code == "audit.no-artifact");
    CHECK(empty.error.find("no auditable Pulp standalone") !=
          std::string::npos);

    const auto plugin = temporary.path / "Danger.vst3" / "Contents" / "MacOS";
    fs::create_directories(plugin);
    write_artifact(plugin / "Danger",
        "PULP_STANDALONE_ARTIFACT_V1 "
        "legacy-handler=view.param_set");
    const auto unauditable = pulp::cli::inspector_shipping::audit_artifact(
        temporary.path / "Danger.vst3");
    REQUIRE_FALSE(unauditable.complete);
    CHECK(unauditable.error_code == "audit.no-artifact");
    CHECK(unauditable.error.find("no auditable Pulp standalone") !=
          std::string::npos);

    const auto missing = pulp::cli::inspector_shipping::audit_artifact(
        temporary.path / "missing-product");
    CHECK_FALSE(missing.complete);
    CHECK(missing.error_code == "audit.target-missing");
}

TEST_CASE("control audit never follows artifact or manifest symlinks") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");

    pulp::inspect::ControlManifest control;
    control.target = "Linked";
    control.product_name = "Linked";
    control.build_id = "build:88888888888888888888888888888888";
    const auto json = pulp::inspect::serialize_control_manifest(control);
    const auto digest = pulp::inspect::control_manifest_digest(control);

    const auto outside_artifact = temporary.path / "outside-artifact";
    write_artifact(outside_artifact, standalone_artifact(
        " PULP_CONTROL_PROFILE_PRODUCTION_STRIPPED_V1 "
        "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1"));
    const auto audit_root = temporary.path / "audit-root";
    fs::create_directories(audit_root);
    write_manifest(audit_root / "Linked.inspector-capabilities.json", json);
    fs::create_symlink(outside_artifact, audit_root / "Linked");

    const auto directory =
        pulp::cli::inspector_shipping::audit_artifact(audit_root);
    CHECK_FALSE(directory.complete);
    CHECK(directory.error_code == "audit.symlink-forbidden");

    const auto direct =
        pulp::cli::inspector_shipping::audit_artifact(audit_root / "Linked");
    CHECK_FALSE(direct.complete);
    CHECK(direct.error_code == "audit.symlink-forbidden");

    fs::remove(audit_root / "Linked");
    write_artifact(audit_root / "Linked", standalone_artifact(
        " PULP_CONTROL_PROFILE_PRODUCTION_STRIPPED_V1 "
        "PULP_CONTROL_MANIFEST_SHA256_" + digest + "_V1"));
    fs::remove(audit_root / "Linked.inspector-capabilities.json");
    const auto outside_manifest = temporary.path / "outside-manifest.json";
    write_manifest(outside_manifest, json);
    fs::create_symlink(outside_manifest,
                       audit_root / "Linked.inspector-capabilities.json");

    const auto sidecar =
        pulp::cli::inspector_shipping::audit_artifact(audit_root / "Linked");
    CHECK_FALSE(sidecar.complete);
    CHECK(sidecar.error_code == "audit.symlink-forbidden");

    const auto directory_sidecar =
        pulp::cli::inspector_shipping::audit_artifact(audit_root);
    CHECK_FALSE(directory_sidecar.complete);
    CHECK(directory_sidecar.error_code == "audit.symlink-forbidden");
}

TEST_CASE("unrelated app bundles do not require standalone evidence") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto executable = temporary.path / "Helpers" / "Preview.app" /
        "Contents" / "MacOS" / "Preview";
    fs::create_directories(executable.parent_path());
    write_artifact(executable, "ordinary helper app");

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK(report.complete);
    CHECK(report.manifests.empty());
}

TEST_CASE("standalone static archives are not runnable shipping artifacts") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto archive = temporary.path / "libpulp-standalone.a";
    write_artifact(archive, standalone_artifact());

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    REQUIRE(report.complete);
    CHECK(report.manifests.empty());
}

TEST_CASE("plugin-like directory names cannot hide inspector artifacts") {
    TemporaryDirectory temporary;
    fs::remove_all(temporary.path / "pulp-inspector-manifests");
    const auto executable = temporary.path / "AU" / "Developer.app" /
        "Contents" / "MacOS" / "Developer";
    fs::create_directories(executable.parent_path());
    write_artifact(executable, standalone_artifact(
        " PULP_INSPECT_SHIPPING_MANIFEST_V1 "
        "PULP_INSPECT_CAPABILITY_UI_READ_V1"));

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error.find("missing inspector capability sidecar") !=
          std::string::npos);
}
