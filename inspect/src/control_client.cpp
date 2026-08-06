#include <pulp/inspect/control_client.hpp>

#include <pulp/inspect/client.hpp>

namespace pulp::inspect {
namespace {

class InspectorControlTransport final : public ControlClientTransport {
  public:
    explicit InspectorControlTransport(InspectorClient& inspector) : inspector_(inspector) {}

    ControlTransportDispatchResult dispatch(std::string_view encoded_envelope,
                                            std::chrono::milliseconds timeout) override {
        const auto response = inspector_.request(std::string(methods::kControlDispatch),
                                                 std::string(encoded_envelope), timeout);
        if (response.is_error) {
            return {
                .error_code = response.error_code,
                .explanation = response.params_json,
            };
        }
        return {.encoded_response = response.params_json};
    }

    ControlArtifactReadResult read_artifact(std::string_view, std::uint64_t, std::size_t,
                                            std::chrono::milliseconds) override {
        return {
            .status = ControlArtifactStatus::IoError,
            .explanation = "artifact reads require the Phase 3c control carrier",
        };
    }

  private:
    InspectorClient& inspector_;
};

} // namespace

ControlClient::ControlClient(ControlClientTransport& transport) : transport_(&transport) {}

ControlClient::ControlClient(InspectorClient& inspector)
    : owned_transport_(std::make_unique<InspectorControlTransport>(inspector)),
      transport_(owned_transport_.get()) {}

ControlClient::~ControlClient() = default;

ControlClient::DispatchResult ControlClient::dispatch(const ControlEnvelope& envelope,
                                                      std::chrono::milliseconds timeout) {
    const auto encoded = encode_control_envelope(envelope);
    if (encoded.empty()) {
        return {
            .error_code = "invalid_control_envelope",
            .explanation = "control envelope is invalid",
        };
    }
    auto response = transport_->dispatch(encoded, timeout);
    if (!response.encoded_response) {
        return {
            .error_code = std::move(response.error_code),
            .explanation = std::move(response.explanation),
        };
    }
    ControlProtocolDiagnostics diagnostics;
    auto decoded = decode_control_envelope(*response.encoded_response, &diagnostics);
    if (!decoded) {
        return {
            .error_code = "invalid_control_response",
            .explanation = diagnostics.explanation,
        };
    }
    return {.response = std::move(decoded)};
}

ControlClientNegotiationResult ControlClient::negotiate(const ControlNegotiationOffer& offer,
                                                        std::chrono::milliseconds timeout) {
    auto result = dispatch(
        {
            .schema_version = kControlProtocolVersion,
            .payload = offer,
        },
        timeout);
    if (!result.response) {
        return {
            .error_code = std::move(result.error_code),
            .explanation = std::move(result.explanation),
        };
    }
    const auto* response = std::get_if<ControlNegotiationResult>(&result.response->payload);
    if (!response) {
        return {
            .error_code = "unexpected_control_response",
            .explanation = "control negotiation returned a non-negotiation response",
        };
    }
    return {.response = *response};
}

ControlClientReceiptResult ControlClient::request(const ControlRequestEnvelope& request,
                                                  std::chrono::milliseconds timeout) {
    auto result = dispatch(
        {
            .schema_version = kControlProtocolVersion,
            .payload = request,
        },
        timeout);
    if (!result.response) {
        return {
            .error_code = std::move(result.error_code),
            .explanation = std::move(result.explanation),
        };
    }
    const auto* response = std::get_if<ControlReceiptEnvelope>(&result.response->payload);
    if (!response) {
        return {
            .error_code = "unexpected_control_response",
            .explanation = "control request returned a non-receipt response",
        };
    }
    return {.response = *response};
}

ControlClientReceiptResult ControlClient::cancel(const ControlCancelEnvelope& cancellation,
                                                 std::chrono::milliseconds timeout) {
    auto result = dispatch(
        {
            .schema_version = kControlProtocolVersion,
            .payload = cancellation,
        },
        timeout);
    if (!result.response) {
        return {
            .error_code = std::move(result.error_code),
            .explanation = std::move(result.explanation),
        };
    }
    const auto* response = std::get_if<ControlReceiptEnvelope>(&result.response->payload);
    if (!response) {
        return {
            .error_code = "unexpected_control_response",
            .explanation = "control cancellation returned a non-receipt response",
        };
    }
    return {.response = *response};
}

ControlArtifactReadResult ControlClient::read_artifact(std::string_view artifact_id,
                                                       std::uint64_t offset,
                                                       std::size_t maximum_bytes,
                                                       std::chrono::milliseconds timeout) {
    return transport_->read_artifact(artifact_id, offset, maximum_bytes, timeout);
}

} // namespace pulp::inspect
