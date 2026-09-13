#pragma once

#include <pulp/inspect/control_execution.hpp>

#include <pulp/timeline/transaction.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect {

/// The action one `dev.pulp.timeline/document-session@1` request selects.
///
/// One capability carries one operation, so the mutable-document surface is a
/// single operation discriminated by action rather than five capabilities that
/// would each need their own grant.
enum class ControlTimelineDocumentSessionAction : std::uint8_t {
    /// Opens a mutable session over a canonical project and registers a writer.
    Open,
    /// Submits one transaction of commands against an open session.
    Apply,
    /// Reads the accumulated change set of an open session.
    Diff,
    /// Reverts the newest closed undo group.
    Undo,
    /// Reapplies the newest redo group.
    Redo,
};

/// Returns the stable public name of `action`, as the JSON schema spells it.
std::string_view
control_timeline_document_session_action_name(ControlTimelineDocumentSessionAction action) noexcept;

/// Resolves an action from its public name, or nullopt for an unknown name.
std::optional<ControlTimelineDocumentSessionAction>
control_timeline_document_session_action_by_name(std::string_view name) noexcept;

/// One decoded document-session request, already validated against the
/// operation's input schema and the broker's writer-profile policy.
struct ControlTimelineDocumentSessionRequest {
    ControlTimelineDocumentSessionAction action = ControlTimelineDocumentSessionAction::Diff;
    /// Canonical project path or identifier. Present only for `Open`.
    std::string project;
    /// Session identity returned by a prior `Open`. Absent for `Open`.
    std::string session_id;
    /// Command payload in the offline surface's own command JSON. `Apply` only.
    std::string commands_json;
    /// Admitted writer-profile name. Never `trusted` when the caller chose it.
    std::string writer_profile;
    /// Caller-supplied replay key. `Apply` only; empty when not supplied.
    std::string idempotency_key;
    /// Optimistic-concurrency precondition. `Apply` only.
    std::optional<std::uint64_t> expected_revision;
};

/// One typed outcome from the bound `timeline::DocumentSession`.
///
/// A refusal carries the session's own `ConflictCode` rather than a broker-side
/// re-interpretation, so the live surface and the offline surface refuse the
/// same case under the same name.
struct ControlTimelineDocumentSessionOutcome {
    /// True when the session accepted the request.
    bool accepted = false;
    /// Session identity. Required on every accepted outcome.
    std::string session_id;
    /// Published document revision after the request.
    std::uint64_t revision = 0;
    /// True when the session committed a new revision.
    bool applied = false;
    /// True when an identical `idempotency_key` replayed a cached result.
    bool replayed = false;
    /// Change-set payload for `Diff`, empty otherwise.
    std::string diff_json;
    /// The session's refusal cause. Required on every refused outcome.
    std::optional<pulp::timeline::ConflictCode> conflict;
    /// The offline surface's typed refusal envelope, verbatim.
    std::string refusal_json;
    /// Human-readable account of the refusal.
    std::string explanation;
};

/// Exact-instance source for one live mutable Timeline document session.
///
/// `invoke` runs synchronously on a legal background thread, never the audio
/// thread, and must not block on the host main thread. It owns the session
/// mechanics; this executor adds no document behavior of its own.
/// Runtime operation metadata, grants, instances, and receipts stay outside the
/// design-time agent-capabilities manifest even though these headers install in
/// the same SDK.
struct ControlTimelineDocumentSessionSource {
    ControlRegistrationId registration_id;
    std::string instance_id;
    std::string publication_id;
    std::function<ControlTimelineDocumentSessionOutcome(
        const ControlTimelineDocumentSessionRequest&)>
        invoke;
};

using ControlTimelineDocumentSessionSourceResolver =
    std::function<std::optional<ControlTimelineDocumentSessionSource>(const ControlAdmissionPlan&)>;

/// Projects a session-side conflict onto the broker result code.
///
/// One-directional by construction: broker policy reads the authority verdict
/// the session already reached and never re-decides it, so there is no second
/// authority model to drift. `CapabilityDenied` is the `CommandAuthority` axis
/// and lands on `PolicyDenied`.
ControlResultCode
control_timeline_conflict_result_code(pulp::timeline::ConflictCode conflict) noexcept;

/// Returns how a caller may retry a request refused with `conflict`.
ControlRetryClassification
control_timeline_conflict_retry(pulp::timeline::ConflictCode conflict) noexcept;

/// Returns the writer-profile name the broker admits for `requested`.
///
/// Returns nullopt for an unknown name and for `trusted`: the broker never
/// selects the unquotaed authority on a caller's behalf and never escalates a
/// caller into it. An empty request resolves to `proposal`, the least
/// authority, rather than to whatever the host happens to hold.
std::optional<std::string>
control_timeline_admissible_writer_profile(std::string_view requested) noexcept;

/// Creates the canonical `dev.pulp.timeline/document-session@1` executor.
///
/// The operation preserves the admission plan's exact registration, instance,
/// publication, deadline, grant, and receipt bindings, and validates every
/// accepted payload against the operation's declared output schema before it
/// publishes one.
ControlOperationExecutor make_control_timeline_document_session_executor(
    ControlTimelineDocumentSessionSourceResolver resolve_source);

} // namespace pulp::inspect
