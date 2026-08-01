#include "tools/cli/inspector_shipping_report.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>

namespace fs = std::filesystem;

namespace {
struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() /
        ("pulp-inspector-shipping-report-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { fs::create_directories(path / "pulp-inspector-manifests"); }
    ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};

void write_manifest(const fs::path& path, std::string_view body) {
    std::ofstream output(path);
    output << body;
}

void write_artifact(const fs::path& path, std::string_view body) {
    std::ofstream output(path, std::ios::binary);
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
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
    write_artifact(artifact, "ordinary-product");
    std::string error;
    CHECK(pulp::cli::inspector_shipping::scan_artifact(
        artifact, report.manifests.front(), error));

    write_artifact(artifact, "PULP_INSPECT_SHIPPING_MANIFEST_V1");
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
    write_artifact(artifact,
        "PULP_INSPECT_SHIPPING_MANIFEST_V1 PULP_INSPECT_CAPABILITY_UI_READ_V1");
    std::string error;
    CHECK(pulp::cli::inspector_shipping::scan_artifact(
        artifact, report.manifests.front(), error));
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
    write_artifact(artifact,
        "third-party InspectorServer DiscoveryPublisher publish_discovery_record");
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
    write_artifact(artifacts / "Alias",
        "PULP_INSPECT_SHIPPING_MANIFEST_V1 PULP_INSPECT_CAPABILITY_STATE_WRITE_V1");
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

    write_artifact(artifacts / "Alias", "ordinary standalone");
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
    write_artifact(executable, "ordinary standalone");

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error.find("missing inspector capability sidecar") !=
          std::string::npos);

    fs::remove(temporary.path / "pulp-inspector-manifests" / "Missing.json");
    const auto unconfigured =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(unconfigured.complete);
    CHECK(unconfigured.error.find("missing inspector capability sidecar") !=
          std::string::npos);
}

TEST_CASE("mixed build trees reject unconfigured standalone artifacts") {
    TemporaryDirectory temporary;
    write_manifest(temporary.path / "pulp-inspector-manifests" / "Current.json",
        R"({"target":"Current","product_name":"Current","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    const auto current_dir =
        temporary.path / "products" / "Current.app" / "Contents/MacOS";
    fs::create_directories(current_dir);
    write_artifact(current_dir / "Current", "ordinary current standalone");
    write_manifest(current_dir / "Current.inspector-capabilities.json",
        R"({"target":"Current","product_name":"Current","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");

    const auto stale_dir =
        temporary.path / "products" / "Removed.app" / "Contents/MacOS";
    fs::create_directories(stale_dir);
    write_artifact(stale_dir / "Removed",
        "PULP_INSPECT_SHIPPING_MANIFEST_V1 PULP_INSPECT_CAPABILITY_UI_READ_V1");

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
    write_artifact(products / "First.exe", "ordinary first standalone");
    write_manifest(products / "First.inspector-capabilities.json",
        R"({"target":"First","product_name":"First","shipping_override":false,"unsafe_runtime_eval_acknowledged":false,"capabilities":[]})");
    write_artifact(products / "Second.exe",
        "PULP_INSPECT_SHIPPING_MANIFEST_V1 PULP_INSPECT_CAPABILITY_UI_READ_V1");

    const auto report =
        pulp::cli::inspector_shipping::load_artifact_report(temporary.path);
    CHECK_FALSE(report.complete);
    CHECK(report.error.find("Second.exe") != std::string::npos);
    CHECK(report.error.find("missing inspector capability sidecar") !=
          std::string::npos);
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
