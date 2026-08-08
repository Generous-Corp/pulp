#pragma once

#include <pulp/inspect/protocol.hpp>

#include <functional>
#include <string_view>

namespace pulp::inspect {

/// Authenticated transport identity attached to a domain request.
struct InspectorRequestContext {
    /// Borrowed for the duration of the handler call; copy it when retaining
    /// per-client state beyond the request.
    std::string_view client_id;
};

/// Outcome of delivering one generation-scoped event to one authenticated
/// transport client. Telemetry brokers use the lossy outcomes as debt carried
/// into their next delivered sample; reliable overflow closes the client.
enum class InspectorTargetedEventResult {
    Queued,
    QueuedAfterLossyEviction,
    DroppedLossy,
    ReliableOverflow,
    ClientNotFound,
    EventUnavailable,
    MessageTooLarge,
};

using InspectorEventSink = std::function<void(const InspectorMessage&)>;
using InspectorTargetedEventSink = std::function<InspectorTargetedEventResult(
    std::string_view client_id, const InspectorMessage& event, std::string_view loss_owner)>;
using InspectorEventRetirementSink = std::function<void(
    std::string_view client_id, std::string_view loss_owner)>;

} // namespace pulp::inspect
