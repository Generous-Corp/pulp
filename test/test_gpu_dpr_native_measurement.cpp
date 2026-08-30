#include <pulp_tooling/gpu_probe/dpr_measurement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace pulp::tooling::gpu_probe::testing {

std::optional<double> run_first_frame_child_time_for_test(
    const DprMeasurementRequest& request,
    const std::filesystem::path& request_path,
    const std::filesystem::path& producer_path,
    const std::filesystem::path& output_path,
    std::string* error);

} // namespace pulp::tooling::gpu_probe::testing

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
        + R"("fresh_process_first_frame_trials":20,"gpu_timer_calibration_trials":5,)"
        + R"("gpu_timer_extra_work_multiplier":8},"scenario":{"id":")"
        + std::string{scenario} + R"(","kind":")" + std::string{kind}
        + (scenario == "threejs-audio-reactive"
            ? R"(","source":"examples/threejs-native-demo/main.cpp","logical_size":{"width":900,"height":600},"logical_input_oracle":{"point":[8,8],"target":"view:pulp-dpr-threejs-canvas"}}})"
            : std::string{R"(","source":"dense-text-thin-strokes.ui.js","logical_size":{"width":640,"height":360},"logical_input_oracle":{"point":[8,8],"target":"view:root"})"}
              + (scenario == "dense-text-thin-strokes"
                  ? R"(,"fidelity_oracle":{"small_text_roi":{"x":24,"y":24,"width":592,"height":105},"thin_stroke_roi":{"x":24,"y":155,"width":592,"height":145}})"
                  : "")
              + "}}");
}

} // namespace

TEST_CASE("native DPR measurement recognizes the three Pulp-owned fixtures",
          "[gpu][dpr][measurement]") {
    for (const auto& [scenario, kind] : {
             std::pair{"dense-text-thin-strokes", "pulp_screenshot"},
             std::pair{"shader-heavy-controls", "pulp_screenshot_gpu"},
             std::pair{"meters-waveforms", "pulp_screenshot_gpu"},
             std::pair{"threejs-audio-reactive", "maintained_native_canary"},
         }) {
        std::string error;
        const auto parsed = probe::parse_dpr_measurement_request(
            request(scenario, kind), &error);
        INFO(error);
        REQUIRE(parsed);
        CHECK(parsed->scenario_id == scenario);
        CHECK(parsed->attempt_number == 1);
        CHECK(parsed->logical_input_x == 8.0);
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
    CHECK(error.find("Pulp-owned native") != std::string::npos);
    CHECK_FALSE(probe::parse_dpr_measurement_request(
        request("dense-text-thin-strokes", "pulp_screenshot", "ABC"), &error));
    CHECK(error.find("32 lowercase") != std::string::npos);
    auto invalid_dpr = request();
    invalid_dpr.replace(invalid_dpr.find("1.0"), 3, "0.0");
    CHECK_FALSE(probe::parse_dpr_measurement_request(invalid_dpr, &error));
    CHECK(error.find("(0, 4]") != std::string::npos);
}

TEST_CASE("fresh-process timing spans launch through acknowledgement but excludes teardown",
          "[gpu][dpr][measurement]") {
    const auto root = std::filesystem::temp_directory_path() /
        ("pulp-dpr-parent-timing-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{root};

    const auto producer = root / "delayed-first-frame.sh";
    const auto output = root / "first-frame.json";
    const auto request_path = root / "request.json";
    std::ofstream(producer, std::ios::trunc)
        << "#!/bin/sh\n"
           "sleep 0.08\n"
           "printf R\n"
           "cat >/dev/null\n"
           "printf '%s\\n' "
           "'{\"attempt_nonce\":\"11111111111111111111111111111111\","
           "\"attempt_number\":1,\"pid\":'\"$$\"',"
           "\"first_frame_time_ms\":0.001}' > \"$4\"\n"
           "sleep 0.40\n";
    std::filesystem::permissions(
        producer,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    const auto parsed = probe::parse_dpr_measurement_request(request());
    REQUIRE(parsed);
    std::string error;
    const auto wall_started = std::chrono::steady_clock::now();
    const auto measured = probe::testing::run_first_frame_child_time_for_test(
        *parsed, request_path, producer, output, &error);
    const auto wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wall_started).count();
    INFO(error);
    REQUIRE(measured);
    CHECK(*measured >= 50.0);
    CHECK(wall_ms - *measured >= 250.0);
}

TEST_CASE("fresh-process handshake survives closed parent stdio",
          "[gpu][dpr][measurement]") {
    const auto root = std::filesystem::temp_directory_path() /
        ("pulp-dpr-closed-stdio-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{root};

    const auto producer = root / "first-frame.sh";
    const auto output = root / "first-frame.json";
    const auto request_path = root / "request.json";
    std::ofstream(producer, std::ios::trunc)
        << "#!/bin/sh\n"
           "printf R\n"
           "cat >/dev/null\n"
           "printf '%s\\n' "
           "'{\"attempt_nonce\":\"11111111111111111111111111111111\","
           "\"attempt_number\":1,\"pid\":'\"$$\"',"
           "\"first_frame_time_ms\":0.001}' > \"$4\"\n";
    std::filesystem::permissions(
        producer,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    int result_pipe[2]{-1, -1};
    REQUIRE(pipe(result_pipe) == 0);
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        close(result_pipe[0]);
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        const auto parsed = probe::parse_dpr_measurement_request(request());
        std::string error;
        const auto measured = parsed
            ? probe::testing::run_first_frame_child_time_for_test(
                  *parsed, request_path, producer, output, &error)
            : std::nullopt;
        const char outcome = measured ? 'P' : 'F';
        (void)write(result_pipe[1], &outcome, sizeof(outcome));
        close(result_pipe[1]);
        _exit(measured ? 0 : 1);
    }
    close(result_pipe[1]);
    char outcome = 0;
    const auto bytes = read(result_pipe[0], &outcome, sizeof(outcome));
    close(result_pipe[0]);
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(bytes == 1);
    CHECK(outcome == 'P');
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
