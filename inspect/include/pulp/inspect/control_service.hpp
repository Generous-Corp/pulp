#pragma once

#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_execution.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pulp::inspect {

enum class ControlServiceStatus : std::uint8_t {
    Responded,
    InvalidEnvelope,
    UnsupportedMessage,
    AdmissionDenied,
    ReceiptTransitionFailed,
    NegotiationRequired,
    ResourceExhausted,
};

std::string_view control_service_status_id(ControlServiceStatus status);

struct ControlServiceResult {
    ControlServiceStatus status = ControlServiceStatus::InvalidEnvelope;
    std::optional<ControlEnvelope> response;
    std::optional<ControlAdmissionStatus> admission_status;
    std::string explanation;
};

struct ControlServiceConfig {
    std::size_t maximum_progress_events_per_operation = 1024;
};

/// Dormant typed dispatcher over the broker composition root.
///
/// The service owns no listener or transport. A carrier verifies the peer and
/// passes its proof plus the bounded encoded envelope here. The injected
/// executor is the only path from admission into operation-specific work.
class ControlService {
  public:
    using ProgressReporter = ControlProgressReporter;
    using ProgressSink = std::function<bool(const ControlProgressEnvelope&)>;
    using Executor = ControlOperationExecutor;

    explicit ControlService(ControlBroker& broker, Executor executor = {},
                            ProgressSink progress_sink = {}, ControlServiceConfig config = {});
    ~ControlService();

    /// Carrier-owned state for exactly one authenticated local connection.
    /// Negotiation cannot leak across peers and is discarded on disconnect by
    /// destroying this value.
    class Session {
      public:
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&& other) noexcept;
        Session& operator=(Session&& other) noexcept;

        ControlServiceResult dispatch(std::string_view encoded_envelope);
        /// Reads only artifacts produced by this connection-bound client ID.
        ControlArtifactReadResult read_artifact(std::string_view artifact_id, std::uint64_t offset,
                                                std::size_t maximum_bytes);

      private:
        friend class ControlService;
        Session(ControlService& service, VerifiedControlPeerIdentity peer,
                ControlClientId client_id)
            : service_(&service), broker_(&service.broker_), peer_(std::move(peer)),
              client_id_(std::move(client_id)) {}

        void close() noexcept;

        ControlService* service_ = nullptr;
        ControlBroker* broker_ = nullptr;
        VerifiedControlPeerIdentity peer_;
        ControlClientId client_id_;
        std::optional<std::vector<std::string>> negotiated_features_;
    };

    Session open_session(const VerifiedControlPeerIdentity& peer,
                         const ControlClientId& connection_client_id) {
        return Session{*this, peer, connection_client_id};
    }

    bool is_listening() const {
        return false;
    }

  private:
    struct CompletionOwner {
        std::mutex mutex;
        ControlBroker* broker = nullptr;
        // The owner mutex is the teardown fence: callbacks hold it across
        // broker settlement, and destruction terminalizes these entries before
        // detaching the broker.
        std::unordered_map<std::string, std::function<void(ControlBroker&)>> deferred_operations;
    };

    ControlServiceResult dispatch(Session& session, std::string_view encoded_envelope);
    ControlServiceResult dispatch_request(Session& session, const ControlRequestEnvelope& request);
    ControlArtifactReadResult read_artifact(Session& session, std::string_view artifact_id,
                                            std::uint64_t offset, std::size_t maximum_bytes);

    ControlBroker& broker_;
    Executor executor_;
    ProgressSink progress_sink_;
    ControlServiceConfig config_;
    std::shared_ptr<CompletionOwner> completion_owner_;
};

} // namespace pulp::inspect
