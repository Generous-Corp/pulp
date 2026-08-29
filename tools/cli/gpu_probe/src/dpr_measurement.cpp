#include <pulp_tooling/gpu_probe/dpr_measurement.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <cmath>
#include <exception>

namespace pulp::tooling::gpu_probe {
namespace {

bool valid_hex(std::string_view value, std::size_t size) {
    if (value.size() != size) return false;
    for (const char character : value)
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f')))
            return false;
    return true;
}

bool read_string(const choc::value::ValueView& object, const char* field,
                 std::string& value, std::string& error) {
    if (!object.hasObjectMember(field) || !object[field].isString()) {
        error = std::string(field) + " is required and must be a string";
        return false;
    }
    value = std::string(object[field].getString());
    return true;
}

bool supported_scenario(std::string_view id, std::string_view kind) {
    return (id == "dense-text-thin-strokes" && kind == "pulp_screenshot") ||
        ((id == "shader-heavy-controls" || id == "meters-waveforms") &&
         kind == "pulp_screenshot_gpu") ||
        (id == "threejs-audio-reactive" && kind == "maintained_native_canary");
}

bool supported_mode(std::string_view mode) {
    return mode == "exact" || mode == "configured_max" ||
        mode == "adaptive_simulation";
}

bool read_positive_u32(const choc::value::ValueView& object, const char* field,
                       std::uint32_t& value, std::string& error) {
    if (!object.hasObjectMember(field) ||
        (!object[field].isInt32() && !object[field].isInt64())) {
        error = std::string(field) + " is required and must be an integer";
        return false;
    }
    const auto parsed = object[field].getInt64();
    if (parsed <= 0 || parsed > 10'000) {
        error = std::string(field) + " must be in [1, 10000]";
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool read_finite_number(const choc::value::ValueView& value, double& result) {
    if (value.isFloat64()) result = value.getFloat64();
    else if (value.isFloat32()) result = value.getFloat32();
    else if (value.isInt32() || value.isInt64())
        result = static_cast<double>(value.getInt64());
    else return false;
    return std::isfinite(result);
}

} // namespace

std::optional<DprMeasurementRequest>
parse_dpr_measurement_request(std::string_view json, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<DprMeasurementRequest> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (const std::exception& exception) {
        return fail(std::string("request is not valid JSON: ") + exception.what());
    }
    if (!root.isObject()) return fail("request must be an object");
    if (!root.hasObjectMember("schema") || !root["schema"].isString() ||
        root["schema"].getString() != kDprCellRequestSchema)
        return fail("request schema is unsupported");
    if (!root.hasObjectMember("version") ||
        ((!root["version"].isInt32() || root["version"].getInt32() != 1) &&
         (!root["version"].isInt64() || root["version"].getInt64() != 1)))
        return fail("request version must be 1");

    DprMeasurementRequest request;
    std::string message;
    if (!read_string(root, "attempt_nonce", request.attempt_nonce, message) ||
        !read_string(root, "cell_key", request.cell_key, message) ||
        !read_string(root, "mode", request.mode, message))
        return fail(message);
    if (!valid_hex(request.attempt_nonce, 32))
        return fail("attempt_nonce must be 32 lowercase hexadecimal characters");
    if (!read_positive_u32(root, "attempt_number", request.attempt_number, message))
        return fail(message);
    if (!supported_mode(request.mode)) return fail("measurement mode is unsupported");

    if (!root.hasObjectMember("requested_dpr"))
        return fail("requested_dpr is required");
    const auto dpr = root["requested_dpr"];
    if (dpr.isFloat64()) request.requested_dpr = dpr.getFloat64();
    else if (dpr.isFloat32()) request.requested_dpr = dpr.getFloat32();
    else if (dpr.isInt32() || dpr.isInt64())
        request.requested_dpr = static_cast<double>(dpr.getInt64());
    else return fail("requested_dpr must be numeric");
    if (!std::isfinite(request.requested_dpr) || request.requested_dpr <= 0.0 ||
        request.requested_dpr > 4.0)
        return fail("requested_dpr must be finite and in (0, 4]");

    if (!root.hasObjectMember("scenario") || !root["scenario"].isObject())
        return fail("scenario is required and must be an object");
    const auto scenario = root["scenario"];
    if (!read_string(scenario, "id", request.scenario_id, message) ||
        !read_string(scenario, "kind", request.scenario_kind, message))
        return fail("scenario." + message);
    if (!supported_scenario(request.scenario_id, request.scenario_kind))
        return fail("scenario is not a Pulp-owned native DPR workload");

    if (!read_string(scenario, "source", request.source, message))
        return fail("scenario." + message);
    if (!scenario.hasObjectMember("logical_size") ||
        !scenario["logical_size"].isObject())
        return fail("scenario.logical_size is required and must be an object");
    const auto logical = scenario["logical_size"];
    if (!read_positive_u32(logical, "width", request.logical_width, message) ||
        !read_positive_u32(logical, "height", request.logical_height, message))
        return fail("scenario.logical_size." + message);
    const bool expected_size = request.scenario_id == "threejs-audio-reactive"
        ? request.logical_width == 900 && request.logical_height == 600
        : request.logical_width == 640 && request.logical_height == 360;
    if (!expected_size)
        return fail("Pulp-native DPR logical size differs from its frozen scenario");
    if (!scenario.hasObjectMember("logical_input_oracle") ||
        !scenario["logical_input_oracle"].isObject())
        return fail("scenario.logical_input_oracle is required");
    const auto oracle = scenario["logical_input_oracle"];
    if (!oracle.hasObjectMember("point") || !oracle["point"].isArray() ||
        oracle["point"].size() != 2 ||
        !read_finite_number(oracle["point"][0], request.logical_input_x) ||
        !read_finite_number(oracle["point"][1], request.logical_input_y) ||
        !read_string(oracle, "target", request.logical_input_target, message))
        return fail("scenario.logical_input_oracle must contain a point and target");
    if (request.logical_input_x < 0 || request.logical_input_y < 0 ||
        request.logical_input_x >= request.logical_width ||
        request.logical_input_y >= request.logical_height ||
        request.logical_input_target != "root-hit")
        return fail("scenario.logical_input_oracle is outside the owned native contract");

    std::string source_root, cell_directory;
    if (!read_string(root, "pulp_source_root", source_root, message) ||
        !read_string(root, "cell_directory", cell_directory, message) ||
        !read_string(root, "expected_content_digest",
                     request.expected_content_digest, message) ||
        !read_string(root, "pulp_sha", request.pulp_sha, message))
        return fail(message);
    request.pulp_source_root = source_root;
    request.cell_directory = cell_directory;
    if (!request.pulp_source_root.is_absolute() ||
        !request.cell_directory.is_absolute())
        return fail("pulp_source_root and cell_directory must be absolute");
    if (request.scenario_id == "threejs-audio-reactive") {
        if (request.source != "examples/threejs-native-demo/main.cpp")
            return fail("Three.js scenario source must remain the maintained native demo");
    } else if (std::filesystem::path(request.source).filename() != request.source) {
        return fail("scenario.source must be one fixture filename");
    }
    if (!valid_hex(request.expected_content_digest, 64) ||
        !valid_hex(request.pulp_sha, 40))
        return fail("content digest or Pulp SHA is malformed");

    if (!root.hasObjectMember("trial_contract") ||
        !root["trial_contract"].isObject())
        return fail("trial_contract is required and must be an object");
    const auto trials = root["trial_contract"];
    if (!read_positive_u32(trials, "warmups", request.warmups, message) ||
        !read_positive_u32(trials, "measured_trials", request.measured_trials,
                           message) ||
        !read_positive_u32(trials, "fresh_process_first_frame_trials",
                           request.fresh_process_first_frame_trials, message) ||
        !read_positive_u32(trials, "gpu_timer_calibration_trials",
                           request.gpu_timer_calibration_trials, message) ||
        !read_positive_u32(trials, "gpu_timer_extra_work_multiplier",
                           request.gpu_timer_extra_work_multiplier, message))
        return fail("trial_contract." + message);
    if (request.warmups != 5 || request.measured_trials != 30 ||
        request.fresh_process_first_frame_trials != 20 ||
        request.gpu_timer_calibration_trials != 5 ||
        request.gpu_timer_extra_work_multiplier != 8)
        return fail("Pulp-native DPR trial counts must remain 5/30/20 with 5x8 timer calibration");
    if (request.mode == "adaptive_simulation") {
        if (!root.hasObjectMember("adaptive_profile") ||
            !root["adaptive_profile"].isObject())
            return fail("adaptive_simulation requires adaptive_profile");
        request.adaptive_profile_json =
            choc::json::toString(root["adaptive_profile"], false);
    }
    return request;
}

DprMeasurementDisposition
evaluate_dpr_measurement_readiness(const DprMeasurementRequest& request) {
    DprMeasurementDisposition result;
    result.request = request;
    result.reason =
        "Pulp-owned APIs do not yet expose one same-process session with authentic "
        "GPU timestamps, resident/upload counters, logical input, capture, and a "
        "correlated five-category Perfetto trace; no terminal samples were emitted";
    result.dependencies = {
        "native-counter:gpu-frame-time",
        "native-counter:resident-bytes",
        "native-counter:upload-bytes",
        "native-input:same-process-logical-oracle",
        "native-trace:correlated-five-category",
    };
    return result;
}

std::string to_json(const DprMeasurementDisposition& result, bool pretty) {
    auto dependencies = choc::value::createEmptyArray();
    for (const auto& dependency : result.dependencies)
        dependencies.addArrayElement(dependency);
    auto root = choc::value::createObject("");
    root.setMember("schema", std::string(kDprCellReceiptSchema));
    root.setMember("version", 1);
    root.setMember("attempt_nonce", result.request.attempt_nonce);
    root.setMember("attempt_number", static_cast<std::int64_t>(result.request.attempt_number));
    root.setMember("scenario_id", result.request.scenario_id);
    root.setMember("scenario_kind", result.request.scenario_kind);
    root.setMember("mode", result.request.mode);
    root.setMember("requested_dpr", result.request.requested_dpr);
    root.setMember("outcome", result.outcome);
    root.setMember("reason", result.reason);
    root.setMember("dependencies", std::move(dependencies));
    return choc::json::toString(root, pretty);
}

} // namespace pulp::tooling::gpu_probe
