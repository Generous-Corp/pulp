#include <pulp_tooling/gpu_probe/dpr_measurement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace probe = pulp::tooling::gpu_probe;

namespace {

std::string request(std::string_view scenario = "dense-text-thin-strokes",
                    std::string_view kind = "pulp_screenshot",
                    std::string_view nonce = "11111111111111111111111111111111") {
    return std::string{R"({"schema":"pulp.gpu-dpr-cell-request.v1","version":1,)"}
        + R"("attempt_nonce":")" + std::string{nonce}
        + R"(","attempt_number":1,"cell_key":"dense-text-thin-strokes__exact__dpr-1",)"
        + R"("mode":"exact","requested_dpr":1.0,)"
        + R"("pulp_source_root":"/pulp","cell_directory":"/cell",)"
        + R"("expected_content_digest":")" + std::string(64, 'a')
        + R"(","pulp_sha":")" + std::string(40, 'b')
        + R"(","trial_contract":{"warmups":5,"measured_trials":30,)"
        + R"("fresh_process_first_frame_trials":20},"scenario":{"id":")"
        + std::string{scenario} + R"(","kind":")" + std::string{kind}
        + R"(","source":"dense-text-thin-strokes.ui.js",)"
        + R"("logical_size":{"width":640,"height":360}}})";
}

} // namespace

TEST_CASE("native DPR measurement recognizes the three Pulp-owned fixtures",
          "[gpu][dpr][measurement]") {
    for (const auto& [scenario, kind] : {
             std::pair{"dense-text-thin-strokes", "pulp_screenshot"},
             std::pair{"shader-heavy-controls", "pulp_screenshot_gpu"},
             std::pair{"meters-waveforms", "pulp_screenshot_gpu"},
         }) {
        std::string error;
        const auto parsed = probe::parse_dpr_measurement_request(
            request(scenario, kind), &error);
        INFO(error);
        REQUIRE(parsed);
        CHECK(parsed->scenario_id == scenario);
        CHECK(parsed->attempt_number == 1);
    }
}

TEST_CASE("native DPR measurement refuses to manufacture terminal counters",
          "[gpu][dpr][measurement]") {
    const auto parsed = probe::parse_dpr_measurement_request(request());
    REQUIRE(parsed);
    const auto result = probe::evaluate_dpr_measurement_readiness(*parsed);
    CHECK(result.outcome == "inconclusive");
    CHECK(result.dependencies.size() == 5);
    const auto json = probe::to_json(result);
    CHECK(json.find("\"outcome\"") != std::string::npos);
    CHECK(json.find("\"inconclusive\"") != std::string::npos);
    CHECK(json.find("gpu-frame-time") != std::string::npos);
    CHECK(json.find("same-process-logical-oracle") != std::string::npos);
    CHECK(json.find("correlated-five-category") != std::string::npos);
    CHECK(json.find("\"metrics\"") == std::string::npos);
    CHECK(json.find("\"measurement_scope\"") == std::string::npos);
}

TEST_CASE("native DPR measurement rejects forged requests",
          "[gpu][dpr][measurement]") {
    std::string error;
    CHECK_FALSE(probe::parse_dpr_measurement_request(
        request("forge-modular-native", "external_forge_canary"), &error));
    CHECK(error.find("three Pulp-native") != std::string::npos);
    CHECK_FALSE(probe::parse_dpr_measurement_request(
        request("dense-text-thin-strokes", "pulp_screenshot", "ABC"), &error));
    CHECK(error.find("32 lowercase") != std::string::npos);
    auto invalid_dpr = request();
    invalid_dpr.replace(invalid_dpr.find("1.0"), 3, "0.0");
    CHECK_FALSE(probe::parse_dpr_measurement_request(invalid_dpr, &error));
    CHECK(error.find("(0, 4]") != std::string::npos);
}
