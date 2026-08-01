#include <pulp/inspect/client_session.hpp>

#include <pulp/inspect/capabilities.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace pulp::inspect {
namespace {

void set_failure(InspectorClientFailure* failure, std::string code, std::string message,
                 std::string data_json = "{}") {
    if (failure) {
        *failure = {std::move(code), std::move(message), std::move(data_json)};
    }
}

bool valid_identity(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
    });
}

bool validate_selection(const InspectorClientSelection& selection,
                        InspectorClientFailure* failure) {
    if (!selection.host.empty() && selection.host != "127.0.0.1" && selection.host != "localhost") {
        set_failure(failure, "invalid_selector", "inspector sessions are loopback-only");
        return false;
    }
    if (selection.port < 0 || selection.port > 65535) {
        set_failure(failure, "invalid_selector",
                    "inspector port selector must be from 1 to 65535 when provided");
        return false;
    }
    for (const auto* value :
         {&selection.session_id, &selection.instance_id, &selection.publication_id}) {
        if (!value->empty() && !valid_identity(*value)) {
            set_failure(failure, "invalid_selector",
                        "inspector identity contains invalid characters");
            return false;
        }
    }
    if (!selection.instance_id.empty() && selection.session_id.empty()) {
        set_failure(failure, "invalid_selector", "instance selection requires a session id");
        return false;
    }
    if (!selection.publication_id.empty() &&
        (selection.session_id.empty() || selection.instance_id.empty())) {
        set_failure(failure, "invalid_selector",
                    "publication selection requires session and instance ids");
        return false;
    }
    return true;
}

std::string endpoint_host(std::string_view endpoint) {
    const auto separator = endpoint.rfind(':');
    return separator == std::string_view::npos ? std::string(endpoint)
                                               : std::string(endpoint.substr(0, separator));
}

int endpoint_port(std::string_view endpoint) {
    const auto separator = endpoint.rfind(':');
    if (separator == std::string_view::npos)
        return 0;
    int value = 0;
    const auto text = endpoint.substr(separator + 1);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? value : 0;
}

enum class ResponseShape { Object, Array };

InspectorMessage validate_typed_response(InspectorMessage response, std::string_view method,
                                         ResponseShape shape) {
    if (response.is_error)
        return response;
    try {
        const auto value = choc::json::parse(response.params_json);
        const bool valid = shape == ResponseShape::Object ? value.isObject() : value.isArray();
        if (valid)
            return response;
    } catch (...) {
    }
    return make_error(response.id, std::string(method) + " returned an invalid response",
                      "invalid_response");
}

} // namespace

std::vector<InspectorDiscoveryRecord>
discover_inspector_sessions(const InspectorClientSelection& selection,
                            InspectorClientFailure* failure,
                            std::filesystem::path runtime_directory) {
    if (failure)
        *failure = {};
    if (!validate_selection(selection, failure))
        return {};

    InspectorDiscoveryReader discovery(std::move(runtime_directory));
    auto records = discovery.list();
    const auto host = selection.host == "localhost" ? std::string("127.0.0.1") : selection.host;
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const auto& record) {
                                     return (!host.empty() &&
                                             endpoint_host(record.endpoint) != host) ||
                                            (selection.port != 0 &&
                                             endpoint_port(record.endpoint) != selection.port) ||
                                            (!selection.session_id.empty() &&
                                             record.session_id != selection.session_id) ||
                                            (!selection.instance_id.empty() &&
                                             record.instance_id != selection.instance_id) ||
                                            (!selection.publication_id.empty() &&
                                             record.publication_id != selection.publication_id);
                                 }),
                  records.end());
    return records;
}

InspectorClientSession::InspectorClientSession(InspectorDiscoveryReader discovery,
                                               InspectorDiscoveryRecord record)
    : discovery_(std::move(discovery)), record_(std::move(record)) {}

InspectorClientSession::~InspectorClientSession() = default;

std::unique_ptr<InspectorClientSession>
InspectorClientSession::connect(const InspectorClientSelection& selection,
                                InspectorClientFailure* failure, std::chrono::milliseconds timeout,
                                std::filesystem::path runtime_directory) {
    if (failure)
        *failure = {};
    if (!validate_selection(selection, failure))
        return nullptr;

    InspectorDiscoveryReader discovery(std::move(runtime_directory));
    auto records = discovery.list();
    const auto host = selection.host == "localhost" ? std::string("127.0.0.1") : selection.host;
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const auto& record) {
                                     return (!host.empty() &&
                                             endpoint_host(record.endpoint) != host) ||
                                            (selection.port != 0 &&
                                             endpoint_port(record.endpoint) != selection.port);
                                 }),
                  records.end());

    std::string selection_error;
    auto selected = select_inspector_session(records, selection.session_id, selection.instance_id,
                                             selection.publication_id, &selection_error);
    if (!selected) {
        set_failure(failure, "session_selection_failed", std::move(selection_error));
        return nullptr;
    }

    auto session = std::unique_ptr<InspectorClientSession>(
        new InspectorClientSession(std::move(discovery), std::move(*selected)));
    InspectorClientConnectFailure connect_failure;
    if (!session->client_.connect(session->record_, session->discovery_, timeout,
                                  &connect_failure)) {
        auto data = choc::value::createObject("");
        data.addMember("sessionId", choc::value::createString(session->record_.session_id));
        data.addMember("instanceId", choc::value::createString(session->record_.instance_id));
        data.addMember("publicationId", choc::value::createString(session->record_.publication_id));
        set_failure(failure,
                    connect_failure.code.empty() ? "connection_failed" : connect_failure.code,
                    connect_failure.message.empty() ? "Inspector connection failed"
                                                    : connect_failure.message,
                    choc::json::toString(data, false));
        return nullptr;
    }
    return session;
}

InspectorMessage InspectorClientSession::request(std::string method, std::string params_json,
                                                 std::chrono::milliseconds timeout) {
    return client_.request(std::move(method), std::move(params_json), timeout);
}

void InspectorClientSession::set_event_handler(EventHandler handler) {
    client_.set_event_handler(std::move(handler));
}

InspectorMessage InspectorClientSession::capabilities(std::chrono::milliseconds timeout) {
    return validate_typed_response(
        request(std::string(methods::kSessionGetCapabilities), "{}", timeout),
        methods::kSessionGetCapabilities, ResponseShape::Object);
}

InspectorMessage InspectorClientSession::agent_context(std::chrono::milliseconds timeout) {
    return validate_typed_response(
        request(std::string(methods::kInspectorGetAgentContext), "{}", timeout),
        methods::kInspectorGetAgentContext, ResponseShape::Object);
}

InspectorMessage InspectorClientSession::parameters(std::chrono::milliseconds timeout) {
    return validate_typed_response(
        request(std::string(methods::kStateGetParameters), "{}", timeout),
        methods::kStateGetParameters, ResponseShape::Array);
}

InspectorMessage InspectorClientSession::set_parameter(std::int64_t parameter_id, double value,
                                                       bool normalized,
                                                       std::chrono::milliseconds timeout) {
    if (parameter_id < 0 ||
        parameter_id > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) ||
        !std::isfinite(value) || (normalized && (value < 0.0 || value > 1.0))) {
        return make_error(0, "Invalid parameter id or value", "invalid_params");
    }
    auto params = choc::value::createObject("");
    params.addMember("id", choc::value::createInt64(parameter_id));
    params.addMember("value", choc::value::createFloat64(value));
    params.addMember("normalized", choc::value::createBool(normalized));
    return request_controlled(std::string(methods::kStateSetParameter),
                              choc::json::toString(params, false), timeout);
}

InspectorMessage InspectorClientSession::screenshot(std::chrono::milliseconds timeout) {
    return validate_typed_response(request(std::string(methods::kCaptureScreenshot), "{}", timeout),
                                   methods::kCaptureScreenshot, ResponseShape::Object);
}

InspectorMessage InspectorClientSession::inject_midi(const MidiTestInput& input,
                                                     std::chrono::milliseconds timeout) {
    if ((input.kind != MidiTestInputKind::NoteOn && input.kind != MidiTestInputKind::NoteOff) ||
        input.channel > 15 || input.note > 127 || input.velocity > 127) {
        return make_error(0, "Invalid bounded MIDI note input", "invalid_params");
    }
    auto params = choc::value::createObject("");
    params.addMember("kind", choc::value::createString(
                                 input.kind == MidiTestInputKind::NoteOn ? "note_on" : "note_off"));
    params.addMember("channel",
                     choc::value::createInt32(static_cast<std::int32_t>(input.channel) + 1));
    params.addMember("note", choc::value::createInt32(input.note));
    params.addMember("velocity", choc::value::createInt32(input.velocity));
    return request_controlled(std::string(methods::kTestInjectMidi),
                              choc::json::toString(params, false), timeout);
}

InspectorMessage InspectorClientSession::set_transport(const StandaloneTransportTestInput& input,
                                                       std::chrono::milliseconds timeout) {
    if ((!input.playing && !input.position_samples && !input.tempo_bpm) ||
        (input.position_samples && *input.position_samples < 0) ||
        (input.tempo_bpm && (!std::isfinite(*input.tempo_bpm) || *input.tempo_bpm < 20.0 ||
                             *input.tempo_bpm > 400.0))) {
        return make_error(0, "Invalid standalone transport input", "invalid_params");
    }
    auto params = choc::value::createObject("");
    if (input.playing)
        params.addMember("playing", choc::value::createBool(*input.playing));
    if (input.position_samples) {
        params.addMember("position_samples", choc::value::createInt64(*input.position_samples));
    }
    if (input.tempo_bpm)
        params.addMember("tempo_bpm", choc::value::createFloat64(*input.tempo_bpm));
    return request_controlled(std::string(methods::kTestSetTransport),
                              choc::json::toString(params, false), timeout);
}

InspectorMessage InspectorClientSession::request_controlled(std::string method,
                                                            std::string params_json,
                                                            std::chrono::milliseconds timeout) {
    const auto* descriptor = find_inspector_method(method);
    if (!descriptor || descriptor->kind != InspectorMethodKind::Request) {
        return make_error(0, "Unknown inspector request method", "method_not_found");
    }
    const bool is_lease_operation = method == methods::kSessionAcquireController ||
                                    method == methods::kSessionRenewController ||
                                    method == methods::kSessionReleaseController;
    if (!is_lease_operation && !capability_requires_controller_lease(descriptor->capability)) {
        return client_.request(std::move(method), std::move(params_json), timeout);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] {
        const auto now = std::chrono::steady_clock::now();
        return now < deadline
                   ? std::max(std::chrono::milliseconds(1),
                              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
                   : std::chrono::milliseconds(0);
    };
    std::unique_lock control_lock(control_mutex_, std::defer_lock);
    if (timeout <= std::chrono::milliseconds(0) || !control_lock.try_lock_until(deadline)) {
        return make_error(0, "Inspector request deadline expired before controller operation",
                          "request_timeout", R"({"mayHaveApplied":false})");
    }
    if (is_lease_operation)
        return client_.request(std::move(method), std::move(params_json), remaining());
    const auto lease =
        client_.request(std::string(methods::kSessionAcquireController), "{}", remaining());
    if (lease.is_error)
        return lease;
    auto response = client_.request(std::move(method), std::move(params_json), remaining());
    const auto release =
        client_.request(std::string(methods::kSessionReleaseController), "{}", remaining());
    if (release.is_error)
        client_.disconnect();
    if (!response.is_error && release.is_error) {
        auto data = choc::value::createObject("");
        data.addMember("mutationApplied", choc::value::createBool(true));
        data.addMember("releaseErrorCode", choc::value::createString(release.error_code));
        data.addMember("releaseError", choc::value::createString(release.params_json));
        return make_error(response.id, "Mutation applied but controller release failed",
                          "controller_release_failed", choc::json::toString(data, false));
    }
    return response;
}

} // namespace pulp::inspect
