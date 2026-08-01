#include <pulp/inspect/client_session.hpp>

#include <pulp/inspect/capabilities.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace pulp::inspect {
namespace {

InspectorClientFailure response_failure(const InspectorMessage& response) {
    return {response.error_code.empty() ? "request_failed" : response.error_code,
            response.params_json.empty() ? "Inspector request failed" : response.params_json,
            response.error_data_json.empty() ? "{}" : response.error_data_json};
}

InspectorClientFailure schema_failure(std::string_view method, std::string_view field) {
    auto data = choc::value::createObject("");
    data.addMember("method", choc::value::createString(method));
    data.addMember("field", choc::value::createString(field));
    return {"invalid_response", std::string(method) + " returned an invalid response",
            choc::json::toString(data, false)};
}

template <typename Value>
InspectorClientResult<Value> failed_result(const InspectorMessage& response) {
    return {std::nullopt, response_failure(response), response.id, {}};
}

template <typename Value>
InspectorClientResult<Value> invalid_result(const InspectorMessage& response,
                                            std::string_view method, std::string_view field) {
    return {std::nullopt, schema_failure(method, field), response.id, {}};
}

template <typename Value>
InspectorClientResult<Value> successful_result(const InspectorMessage& response, Value value) {
    return {std::move(value), {}, response.id, response.params_json};
}

bool is_number(const choc::value::ValueView& value) {
    return value.isInt() || value.isFloat();
}

bool read_nonnegative_u64(const choc::value::ValueView& value, std::uint64_t& output) {
    if (!value.isInt())
        return false;
    const auto signed_value = value.getInt64();
    if (signed_value < 0)
        return false;
    output = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool read_capability_list(const choc::value::ValueView& value,
                          std::vector<InspectorCapability>& output) {
    if (!value.isArray())
        return false;
    for (std::uint32_t index = 0; index < value.size(); ++index) {
        if (!value[index].isString())
            return false;
        const auto capability = capability_from_id(value[index].getString());
        if (!capability || std::find(output.begin(), output.end(), *capability) != output.end())
            return false;
        output.push_back(*capability);
    }
    return true;
}

} // namespace

InspectorClientResult<InspectorCapabilitiesResult>
InspectorClientSession::read_capabilities(std::chrono::milliseconds timeout) {
    const auto response = capabilities(timeout);
    if (response.is_error)
        return failed_result<InspectorCapabilitiesResult>(response);
    try {
        const auto value = choc::json::parse(response.params_json);
        InspectorCapabilitiesResult result;
        if (!value.isObject() || !value["sessionId"].isString() ||
            !value["instanceId"].isString() || !value["pluginId"].isString() ||
            !value["protocolVersion"].isString() || !value["profile"].isString()) {
            return invalid_result<InspectorCapabilitiesResult>(
                response, methods::kSessionGetCapabilities, "identity/profile");
        }
        result.session_id = std::string(value["sessionId"].getString());
        result.instance_id = std::string(value["instanceId"].getString());
        result.plugin_id = std::string(value["pluginId"].getString());
        result.protocol_version = std::string(value["protocolVersion"].getString());
        const auto profile = profile_from_id(value["profile"].getString());
        if (!profile || result.session_id.empty() || result.instance_id.empty() ||
            result.plugin_id.empty() || result.protocol_version.empty() ||
            !read_capability_list(value["available"], result.available) ||
            !read_capability_list(value["effective"], result.effective)) {
            return invalid_result<InspectorCapabilitiesResult>(
                response, methods::kSessionGetCapabilities, "profile/capabilities");
        }
        result.profile = *profile;
        if (result.session_id != record_.session_id || result.instance_id != record_.instance_id ||
            result.plugin_id != record_.plugin_id ||
            result.protocol_version != record_.protocol_version ||
            std::any_of(result.effective.begin(), result.effective.end(),
                        [&](const auto capability) {
                            return std::find(result.available.begin(), result.available.end(),
                                             capability) == result.available.end();
                        })) {
            return invalid_result<InspectorCapabilitiesResult>(
                response, methods::kSessionGetCapabilities, "authenticatedIdentity/policy");
        }
        if (value.hasObjectMember("controller")) {
            if (!value["controller"].isString())
                return invalid_result<InspectorCapabilitiesResult>(
                    response, methods::kSessionGetCapabilities, "controller");
            result.controller = std::string(value["controller"].getString());
        }
        return successful_result(response, std::move(result));
    } catch (...) {
        return invalid_result<InspectorCapabilitiesResult>(
            response, methods::kSessionGetCapabilities, "payload");
    }
}

InspectorClientResult<InspectorAgentContextResult>
InspectorClientSession::read_agent_context(std::chrono::milliseconds timeout) {
    const auto response = agent_context(timeout);
    if (response.is_error)
        return failed_result<InspectorAgentContextResult>(response);
    try {
        const auto value = choc::json::parse(response.params_json);
        const auto binary = value["binary"];
        const auto identity = value["identity"];
        const auto editor = value["editor"];
        const auto processing = value["processing"];
        const auto reload = value["hotReload"];
        const auto issues = value["actionableIssues"];
        if (!value.isObject() || !binary.isObject() || !binary["path"].isString() ||
            !binary["buildId"].isString() || !binary["mtimeUnixMs"].isInt() ||
            !identity.isObject() || !identity["pluginId"].isString() ||
            !identity["sessionId"].isString() || !identity["instanceId"].isString() ||
            !editor.isObject() || !editor["open"].isBool() || !editor["windowVisible"].isBool() ||
            !processing.isObject() || !processing["active"].isBool() || !reload.isObject() ||
            !reload["available"].isBool() || !reload["enabled"].isBool() ||
            !reload["pending"].isBool() || !issues.isArray()) {
            return invalid_result<InspectorAgentContextResult>(
                response, methods::kInspectorGetAgentContext, "payload");
        }
        InspectorAgentContextResult result;
        std::uint64_t xrun_count = 0;
        std::uint64_t tweak_count = 0;
        if (!read_nonnegative_u64(processing["xrunCount"], xrun_count) ||
            !read_nonnegative_u64(value["unsavedTweakCount"], tweak_count)) {
            return invalid_result<InspectorAgentContextResult>(
                response, methods::kInspectorGetAgentContext, "counts");
        }
        result.binary_path = std::string(binary["path"].getString());
        result.binary_build_id = std::string(binary["buildId"].getString());
        result.binary_mtime_unix_ms = binary["mtimeUnixMs"].getInt64();
        result.plugin_id = std::string(identity["pluginId"].getString());
        result.session_id = std::string(identity["sessionId"].getString());
        result.instance_id = std::string(identity["instanceId"].getString());
        if (result.plugin_id != record_.plugin_id || result.session_id != record_.session_id ||
            result.instance_id != record_.instance_id || result.binary_mtime_unix_ms < 0) {
            return invalid_result<InspectorAgentContextResult>(
                response, methods::kInspectorGetAgentContext, "authenticatedIdentity");
        }
        result.editor_open = editor["open"].getBool();
        result.window_visible = editor["windowVisible"].getBool();
        result.processing = processing["active"].getBool();
        result.xrun_count = xrun_count;
        result.hot_reload_available = reload["available"].getBool();
        result.hot_reload_enabled = reload["enabled"].getBool();
        result.hot_reload_pending = reload["pending"].getBool();
        result.unsaved_tweak_count = tweak_count;
        for (std::uint32_t index = 0; index < issues.size(); ++index) {
            if (!issues[index].isString())
                return invalid_result<InspectorAgentContextResult>(
                    response, methods::kInspectorGetAgentContext, "actionableIssues");
            result.actionable_issues.emplace_back(issues[index].getString());
        }
        return successful_result(response, std::move(result));
    } catch (...) {
        return invalid_result<InspectorAgentContextResult>(
            response, methods::kInspectorGetAgentContext, "payload");
    }
}

InspectorClientResult<std::vector<InspectorParameterSnapshot>>
InspectorClientSession::read_parameters(std::chrono::milliseconds timeout) {
    const auto response = parameters(timeout);
    if (response.is_error)
        return failed_result<std::vector<InspectorParameterSnapshot>>(response);
    try {
        const auto value = choc::json::parse(response.params_json);
        if (!value.isArray())
            return invalid_result<std::vector<InspectorParameterSnapshot>>(
                response, methods::kStateGetParameters, "payload");
        std::vector<InspectorParameterSnapshot> result;
        result.reserve(value.size());
        for (std::uint32_t index = 0; index < value.size(); ++index) {
            const auto entry = value[index];
            if (!entry.isObject() || !entry["id"].isInt() || !entry["name"].isString() ||
                !entry["unit"].isString() || !is_number(entry["value"]) ||
                !is_number(entry["normalized"]) || !is_number(entry["modulated"]) ||
                !is_number(entry["default"]) || !is_number(entry["min"]) ||
                !is_number(entry["max"])) {
                return invalid_result<std::vector<InspectorParameterSnapshot>>(
                    response, methods::kStateGetParameters, "parameters[]");
            }
            const auto id = entry["id"].getInt64();
            InspectorParameterSnapshot snapshot;
            snapshot.value = entry["value"].getWithDefault(0.0);
            snapshot.normalized = entry["normalized"].getWithDefault(0.0);
            snapshot.modulated = entry["modulated"].getWithDefault(0.0);
            snapshot.default_value = entry["default"].getWithDefault(0.0);
            snapshot.min = entry["min"].getWithDefault(0.0);
            snapshot.max = entry["max"].getWithDefault(0.0);
            if (id < 0 || id > std::numeric_limits<std::uint32_t>::max() ||
                !std::isfinite(snapshot.value) || !std::isfinite(snapshot.normalized) ||
                !std::isfinite(snapshot.modulated) || !std::isfinite(snapshot.default_value) ||
                !std::isfinite(snapshot.min) || !std::isfinite(snapshot.max) ||
                snapshot.min > snapshot.max) {
                return invalid_result<std::vector<InspectorParameterSnapshot>>(
                    response, methods::kStateGetParameters, "parameters[]");
            }
            snapshot.id = static_cast<std::uint32_t>(id);
            snapshot.name = std::string(entry["name"].getString());
            snapshot.unit = std::string(entry["unit"].getString());
            if (entry.hasObjectMember("display")) {
                if (!entry["display"].isString())
                    return invalid_result<std::vector<InspectorParameterSnapshot>>(
                        response, methods::kStateGetParameters, "parameters[].display");
                snapshot.display = std::string(entry["display"].getString());
            }
            result.push_back(std::move(snapshot));
        }
        return successful_result(response, std::move(result));
    } catch (...) {
        return invalid_result<std::vector<InspectorParameterSnapshot>>(
            response, methods::kStateGetParameters, "payload");
    }
}

InspectorClientResult<InspectorSetParameterResult>
InspectorClientSession::set_parameter_typed(std::int64_t parameter_id, double value,
                                            bool normalized, std::chrono::milliseconds timeout) {
    const auto response = set_parameter(parameter_id, value, normalized, timeout);
    if (response.is_error)
        return failed_result<InspectorSetParameterResult>(response);
    try {
        const auto result = choc::json::parse(response.params_json);
        if (!result.isObject() || !result["ok"].isBool() || !result["ok"].getBool())
            return invalid_result<InspectorSetParameterResult>(response,
                                                               methods::kStateSetParameter, "ok");
        return successful_result(response, InspectorSetParameterResult{true});
    } catch (...) {
        return invalid_result<InspectorSetParameterResult>(response, methods::kStateSetParameter,
                                                           "payload");
    }
}

InspectorClientResult<InspectorScreenshotResult>
InspectorClientSession::capture_screenshot(std::chrono::milliseconds timeout) {
    const auto response = screenshot(timeout);
    if (response.is_error)
        return failed_result<InspectorScreenshotResult>(response);
    try {
        const auto value = choc::json::parse(response.params_json);
        if (!value.isObject() || !value["mimeType"].isString() || !value["width"].isInt() ||
            !value["height"].isInt() || !value["data"].isString()) {
            return invalid_result<InspectorScreenshotResult>(response, methods::kCaptureScreenshot,
                                                             "payload");
        }
        const auto width = value["width"].getInt64();
        const auto height = value["height"].getInt64();
        InspectorScreenshotResult result;
        result.mime_type = std::string(value["mimeType"].getString());
        result.data_base64 = std::string(value["data"].getString());
        if (result.mime_type != "image/png" || result.data_base64.empty() || width <= 0 ||
            height <= 0 || width > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::uint32_t>::max()) {
            return invalid_result<InspectorScreenshotResult>(response, methods::kCaptureScreenshot,
                                                             "image");
        }
        result.width = static_cast<std::uint32_t>(width);
        result.height = static_cast<std::uint32_t>(height);
        return successful_result(response, std::move(result));
    } catch (...) {
        return invalid_result<InspectorScreenshotResult>(response, methods::kCaptureScreenshot,
                                                         "payload");
    }
}

InspectorClientResult<InspectorInjectMidiResult>
InspectorClientSession::inject_midi_typed(const MidiTestInput& input,
                                          std::chrono::milliseconds timeout) {
    const auto response = inject_midi(input, timeout);
    if (response.is_error)
        return failed_result<InspectorInjectMidiResult>(response);
    try {
        const auto value = choc::json::parse(response.params_json);
        if (!value.isObject() || value.size() != 1 || !value["accepted"].isBool() ||
            !value["accepted"].getBool()) {
            return invalid_result<InspectorInjectMidiResult>(response, methods::kTestInjectMidi,
                                                             "accepted");
        }
        return successful_result(response, InspectorInjectMidiResult{true});
    } catch (...) {
        return invalid_result<InspectorInjectMidiResult>(response, methods::kTestInjectMidi,
                                                         "payload");
    }
}

InspectorClientResult<InspectorSetTransportResult>
InspectorClientSession::set_transport_typed(const StandaloneTransportTestInput& input,
                                            std::chrono::milliseconds timeout) {
    const auto response = set_transport(input, timeout);
    if (response.is_error)
        return failed_result<InspectorSetTransportResult>(response);
    try {
        const auto value = choc::json::parse(response.params_json);
        if (!value.isObject() || value.size() != 1 || !value["applied"].isBool() ||
            !value["applied"].getBool()) {
            return invalid_result<InspectorSetTransportResult>(response, methods::kTestSetTransport,
                                                               "applied");
        }
        return successful_result(response, InspectorSetTransportResult{true});
    } catch (...) {
        return invalid_result<InspectorSetTransportResult>(response, methods::kTestSetTransport,
                                                           "payload");
    }
}

} // namespace pulp::inspect
