#pragma once

#include <pulp/inspect/control_artifacts.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect {

class InspectorClient;

struct ControlTransportDispatchResult {
    std::optional<std::string> encoded_response;
    std::string error_code;
    std::string explanation;

    bool succeeded() const {
        return encoded_response.has_value();
    }
};

/// One authenticated, connection-bound capability-control session.
///
/// The transport owns the peer and client lineage established by its carrier.
/// Callers cannot supply or override that identity when reading an artifact.
/// Carrier integrations inject this transport so the installed client remains
/// independent of any particular listener or connection implementation.
class ControlClientTransport {
  public:
    virtual ~ControlClientTransport() = default;

    virtual ControlTransportDispatchResult dispatch(std::string_view encoded_envelope,
                                                    std::chrono::milliseconds timeout) = 0;
    virtual ControlArtifactReadResult read_artifact(std::string_view artifact_id,
                                                    std::uint64_t offset, std::size_t maximum_bytes,
                                                    std::chrono::milliseconds timeout) = 0;
};

template <typename Response> struct ControlDispatchResult {
    std::optional<Response> response;
    std::string error_code;
    std::string explanation;

    bool succeeded() const {
        return response.has_value();
    }
};

using ControlClientNegotiationResult = ControlDispatchResult<ControlNegotiationResult>;
using ControlClientReceiptResult = ControlDispatchResult<ControlReceiptEnvelope>;

/// Typed capability-control client over one connection-bound transport.
///
/// This class neither discovers nor opens a connection. The transport binds
/// negotiation, requests, cancellation, and artifact reads to the same
/// authenticated session and client identity. The InspectorClient constructor
/// is a compatibility adapter for envelope dispatch only; artifact reads fail
/// closed unless the client uses a typed, connection-bound transport.
class ControlClient {
  public:
    /// The connection-bound transport must outlive this client.
    explicit ControlClient(ControlClientTransport& transport);
    explicit ControlClient(InspectorClient& inspector);
    ~ControlClient();

    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;

    ControlClientNegotiationResult
    negotiate(const ControlNegotiationOffer& offer,
              std::chrono::milliseconds timeout = std::chrono::seconds(3));
    ControlClientReceiptResult request(const ControlRequestEnvelope& request,
                                       std::chrono::milliseconds timeout = std::chrono::seconds(3));
    ControlClientReceiptResult cancel(const ControlCancelEnvelope& cancellation,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(3));
    ControlArtifactReadResult
    read_artifact(std::string_view artifact_id, std::uint64_t offset, std::size_t maximum_bytes,
                  std::chrono::milliseconds timeout = std::chrono::seconds(3));

  private:
    struct DispatchResult {
        std::optional<ControlEnvelope> response;
        std::string error_code;
        std::string explanation;
    };

    DispatchResult dispatch(const ControlEnvelope& envelope, std::chrono::milliseconds timeout);
    std::unique_ptr<ControlClientTransport> owned_transport_;
    ControlClientTransport* transport_ = nullptr;
};

} // namespace pulp::inspect
