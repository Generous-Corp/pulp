#include <pulp/inspect/control_client_connection.hpp>

namespace pulp::inspect {

ControlBrokerHealthProbeResult probe_control_broker(const ControlHealthProbeConfig& config,
                                                    std::chrono::milliseconds response_timeout) {
    ControlClientConnection connection(config);
    if (!connection.connect()) {
        return {
            .status = connection.carrier_was_reached()
                          ? ControlBrokerHealthProbeStatus::ReachableUnverified
                          : ControlBrokerHealthProbeStatus::Unavailable,
            .error_code = connection.last_error_code(),
            .explanation = connection.last_error_explanation(),
        };
    }
    if (!config.expected_broker || !connection.broker_was_verified()) {
        connection.disconnect();
        return {
            .status = ControlBrokerHealthProbeStatus::ReachableUnverified,
            .error_code = "broker-expectation-required",
            .explanation = "the carrier is reachable but its broker identity was not verified",
        };
    }

    constexpr std::string_view request_id = "health-probe";
    auto dispatched = connection.dispatch_probe(
        ControlEnvelope{.payload = ControlHealthEnvelope{.request_id = std::string(request_id)}},
        response_timeout);
    if (!dispatched.encoded_response) {
        const bool unavailable = dispatched.error_code == "timeout" ||
                                 dispatched.error_code == "connection-lost" ||
                                 dispatched.error_code == "send-failed";
        connection.disconnect();
        return {
            .status = unavailable ? ControlBrokerHealthProbeStatus::Unavailable
                                  : ControlBrokerHealthProbeStatus::Malformed,
            .error_code = std::move(dispatched.error_code),
            .explanation = std::move(dispatched.explanation),
        };
    }
    ControlProtocolDiagnostics diagnostics;
    const auto envelope = decode_control_envelope(*dispatched.encoded_response, &diagnostics);
    const auto* response =
        envelope ? std::get_if<ControlHealthResult>(&envelope->payload) : nullptr;
    if (!response || response->request_id != request_id) {
        connection.disconnect();
        return {
            .status = ControlBrokerHealthProbeStatus::Malformed,
            .error_code = "malformed-health-response",
            .explanation = envelope ? "the broker returned an unrelated health response"
                                    : diagnostics.explanation,
        };
    }

    ControlBrokerHealth health{
        .sdk_version = response->sdk_version,
        .protocol_versions = response->protocol_versions,
        .broker_id = response->broker_id,
        .process_generation = response->process_generation,
    };
    const bool compatible = response->protocol_versions.minimum <= kControlProtocolVersion &&
                            response->protocol_versions.maximum >= kControlProtocolVersion;
    connection.disconnect();
    return {
        .status = compatible ? ControlBrokerHealthProbeStatus::HealthyVerified
                             : ControlBrokerHealthProbeStatus::Incompatible,
        .health = std::move(health),
        .error_code = compatible ? std::string{} : "incompatible-protocol",
        .explanation = compatible ? std::string{}
                                  : "the broker does not support this control protocol version",
    };
}

} // namespace pulp::inspect
