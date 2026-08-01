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
