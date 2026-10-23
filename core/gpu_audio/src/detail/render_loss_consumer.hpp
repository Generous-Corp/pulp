#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "shared_io_execution_contract.hpp"

namespace pulp::gpu_audio::detail {

// Consumer side of a render-owner device-loss event. Nothing in Pulp produces
// this event: the producer is the Vellum render owner, which does not exist
// yet. This header proves only Pulp's own refusal logic over a
// backend-neutral event struct. It does not observe a render device, does not
// call into the render surface, and is not evidence that a render loss reaches
// GPU audio.
//
// The rules follow the 2026-10-21 Vellum render-owner handoff package: the
// event contract (section 3), cursor rules H1-H12 (section 4), replay ordering
// (section 7) and quiescence (section 8). Where that package and the earlier
// v1 vector set disagree, the package governs.
//
// Threading: install(), apply(), handoff(), close() and classify_completion()
// run on one owner strand that is never the audio callback. The audio callback
// may read admission_ticket() and admission_ticket_current(), which are single
// atomic loads. Every member is allocation-free.

inline constexpr std::uint32_t kRenderOwnerLossContractVersion = 1;

// Fixed-capacity opaque identifier so the cursor never allocates. A value that
// does not fit is kept as overflowed and is never valid, so an identity cannot
// be truncated into a collision with another one.
class RenderLossText {
  public:
    static constexpr std::size_t kCapacity = 64;

    constexpr RenderLossText() noexcept = default;

    static constexpr RenderLossText from(std::string_view text) noexcept {
        RenderLossText out;
        if (text.size() > kCapacity) {
            out.overflowed_ = true;
            return out;
        }
        for (std::size_t i = 0; i < text.size(); ++i)
            out.bytes_[i] = text[i];
        out.size_ = static_cast<std::uint8_t>(text.size());
        return out;
    }

    constexpr std::string_view view() const noexcept {
        return {bytes_.data(), size_};
    }
    constexpr bool valid() const noexcept {
        return size_ != 0 && !overflowed_;
    }

    friend constexpr bool operator==(const RenderLossText&, const RenderLossText&) = default;

  private:
    std::array<char, kCapacity> bytes_{};
    std::uint8_t size_ = 0;
    bool overflowed_ = false;
};

using RenderLossDigest = std::array<std::uint8_t, 32>;

struct RenderOwnerIdentity {
    RenderLossText provider_id;
    RenderLossText provider_version;
    RenderLossText owner_id;

    constexpr bool valid() const noexcept {
        return provider_id.valid() && provider_version.valid() && owner_id.valid();
    }
    friend constexpr bool operator==(const RenderOwnerIdentity&,
                                     const RenderOwnerIdentity&) = default;
};

enum class RenderOwnerLossEventKind : std::uint8_t { Loss, ReplayResult };

enum class RenderOwnerLossReason : std::uint8_t {
    DeviceLost,
    DriverReset,
    OutOfMemory,
    ProviderRefused,
    OwnerTeardown,
    // Valid input, but it can never authorize a replay success.
    Unknown,
};

enum class RenderOwnerLossDisposition : std::uint8_t { Recoverable, Terminal };

// Three states only. The v1 vectors' fourth state, torn_down, is a terminal
// disposition with terminal kind OwnerTeardown, not a quiescence state.
enum class RenderOwnerQuiescence : std::uint8_t { Active, Draining, Quiescent };

enum class RenderOwnerReplay : std::uint8_t { NotAttempted, Succeeded, Failed };

enum class RenderOwnerTerminalKind : std::uint8_t {
    DeviceLost,
    ReplayFailed,
    ProviderRefused,
    OwnerTeardown,
};

struct RenderResourceManifest {
    RenderLossText schema_version;
    RenderLossDigest digest_sha256{};

    friend constexpr bool operator==(const RenderResourceManifest&,
                                     const RenderResourceManifest&) = default;
};

struct RenderOwnerTerminalBlock {
    RenderOwnerTerminalKind kind = RenderOwnerTerminalKind::DeviceLost;
    bool quiescent = false;
    bool callbacks_suppressed = false;
    RenderOwnerReplay replay = RenderOwnerReplay::NotAttempted;

    friend constexpr bool operator==(const RenderOwnerTerminalBlock&,
                                     const RenderOwnerTerminalBlock&) = default;
};

// One event as delivered by the producer. Generations and epochs are non-zero;
// a next_* value of zero means the field is absent.
struct RenderOwnerLossEvent {
    std::uint32_t contract_version = kRenderOwnerLossContractVersion;
    RenderOwnerIdentity identity;
    std::uint64_t owner_generation = 0;
    std::uint64_t surface_epoch = 0;
    RenderLossText event_id;
    RenderOwnerLossEventKind kind = RenderOwnerLossEventKind::Loss;
    std::uint64_t event_seq = 0;
    RenderOwnerLossReason reason = RenderOwnerLossReason::Unknown;
    RenderOwnerLossDisposition disposition = RenderOwnerLossDisposition::Recoverable;
    RenderOwnerQuiescence quiescence = RenderOwnerQuiescence::Active;
    bool callbacks_suppressed = false;
    RenderOwnerReplay replay = RenderOwnerReplay::NotAttempted;
    std::uint64_t next_owner_generation = 0;
    std::uint64_t next_surface_epoch = 0;
    RenderResourceManifest resource_manifest;
    RenderLossDigest payload_digest_sha256{};
    bool has_terminal = false;
    RenderOwnerTerminalBlock terminal;

    friend constexpr bool operator==(const RenderOwnerLossEvent&,
                                     const RenderOwnerLossEvent&) = default;
};

// H9: the only way to install a different owner on an installed cursor.
struct RenderOwnerHandoff {
    std::uint32_t contract_version = kRenderOwnerLossContractVersion;
    RenderOwnerIdentity from;
    RenderOwnerIdentity to;
};

enum class RenderLossRefusal : std::uint8_t {
    None,
    UnknownContractVersion,
    InvalidEnumValue,
    MalformedEvent,
    NoOwnerInstalled,
    ImplicitResetForbidden,
    UnexpectedProviderIdentity,
    UnexpectedOwnerIdentity,
    OwnerReuseWithoutExplicitHandoff,
    EventAfterTerminal,
    CursorResyncRequired,
    StaleSequence,
    SequenceGap,
    ConflictingPayloadForSequence,
    StaleGeneration,
    StaleSurfaceEpoch,
    QuiescentRequiresCallbacksSuppressed,
    TerminalRequiresQuiescentAndCallbacksSuppressed,
    InvalidTerminalBlock,
    ReasonTerminalKindMismatch,
    InvalidLossReplayField,
    InvalidReplayOutcome,
    QuiescenceRegression,
    DuplicateLossEventId,
    InterleavedLossDuringReplay,
    ReplayWithoutLoss,
    DuplicateReplayResult,
    ReplayEventIdMismatch,
    ReplayIdentityMismatch,
    ReplayGenerationMismatch,
    ResourceManifestIdentityChanged,
    ResourceManifestDigestChanged,
    ReplaySuccessRequiresNewGenerationAndSurfaceEpoch,
    ReplaySuccessRequiresKnownReason,
    HandoffFromMismatch,
    HandoffToRetiredOwner,
    HandoffCapacityExhausted,
    HandoffDuringPendingReplay,
    AdmissionTicketsExhausted,
    CompletionFromLostGeneration,
    CompletionFromUnknownGeneration,
    CompletionWhileFenced,
    CompletionFromRetiredOwner,
    CallbackAfterTeardown,
    CompletionAfterCursorClosed,
    CompletionBeforeFirstEvent,
    CompletionWithoutGeneration,
};

// Names match the package and v1 vector reason strings where one exists.
constexpr std::string_view to_string(RenderLossRefusal refusal) noexcept {
    switch (refusal) {
    case RenderLossRefusal::None:
        return "none";
    case RenderLossRefusal::UnknownContractVersion:
        return "unknown_contract_version";
    case RenderLossRefusal::InvalidEnumValue:
        return "invalid_enum_value";
    case RenderLossRefusal::MalformedEvent:
        return "malformed_event";
    case RenderLossRefusal::NoOwnerInstalled:
        return "no_owner_installed";
    case RenderLossRefusal::ImplicitResetForbidden:
        return "implicit_reset_forbidden";
    case RenderLossRefusal::UnexpectedProviderIdentity:
        return "unexpected_provider_identity";
    case RenderLossRefusal::UnexpectedOwnerIdentity:
        return "unexpected_owner_identity";
    case RenderLossRefusal::OwnerReuseWithoutExplicitHandoff:
        return "owner_reuse_without_explicit_handoff";
    case RenderLossRefusal::EventAfterTerminal:
        return "event_after_terminal";
    case RenderLossRefusal::CursorResyncRequired:
        return "cursor_resync_required";
    case RenderLossRefusal::StaleSequence:
        return "stale_sequence";
    case RenderLossRefusal::SequenceGap:
        return "sequence_gap";
    case RenderLossRefusal::ConflictingPayloadForSequence:
        return "conflicting_payload_for_sequence";
    case RenderLossRefusal::StaleGeneration:
        return "stale_generation";
    case RenderLossRefusal::StaleSurfaceEpoch:
        return "stale_surface_epoch";
    case RenderLossRefusal::QuiescentRequiresCallbacksSuppressed:
        return "quiescent_requires_callbacks_suppressed";
    case RenderLossRefusal::TerminalRequiresQuiescentAndCallbacksSuppressed:
        return "terminal_requires_quiescent_and_callbacks_suppressed";
    case RenderLossRefusal::InvalidTerminalBlock:
        return "invalid_terminal_block";
    case RenderLossRefusal::ReasonTerminalKindMismatch:
        return "reason_terminal_kind_mismatch";
    case RenderLossRefusal::InvalidLossReplayField:
        return "invalid_loss_replay_field";
    case RenderLossRefusal::InvalidReplayOutcome:
        return "invalid_replay_outcome";
    case RenderLossRefusal::QuiescenceRegression:
        return "quiescence_regression";
    case RenderLossRefusal::DuplicateLossEventId:
        return "duplicate_loss_event_id";
    case RenderLossRefusal::InterleavedLossDuringReplay:
        return "interleaved_loss_during_replay";
    case RenderLossRefusal::ReplayWithoutLoss:
        return "replay_without_loss";
    case RenderLossRefusal::DuplicateReplayResult:
        return "duplicate_replay_result";
    case RenderLossRefusal::ReplayEventIdMismatch:
        return "replay_event_id_mismatch";
    case RenderLossRefusal::ReplayIdentityMismatch:
        return "replay_identity_mismatch";
    case RenderLossRefusal::ReplayGenerationMismatch:
        return "replay_generation_mismatch";
    case RenderLossRefusal::ResourceManifestIdentityChanged:
        return "resource_manifest_identity_changed";
    case RenderLossRefusal::ResourceManifestDigestChanged:
        return "resource_manifest_digest_changed";
    case RenderLossRefusal::ReplaySuccessRequiresNewGenerationAndSurfaceEpoch:
        return "replay_success_requires_new_generation_and_surface_epoch";
    case RenderLossRefusal::ReplaySuccessRequiresKnownReason:
        return "replay_success_requires_known_reason";
    case RenderLossRefusal::HandoffFromMismatch:
        return "handoff_from_mismatch";
    case RenderLossRefusal::HandoffToRetiredOwner:
        return "handoff_to_retired_owner";
    case RenderLossRefusal::HandoffCapacityExhausted:
        return "handoff_capacity_exhausted";
    case RenderLossRefusal::AdmissionTicketsExhausted:
        return "admission_tickets_exhausted";
    case RenderLossRefusal::CompletionFromLostGeneration:
        return "completion_from_lost_generation";
    case RenderLossRefusal::CompletionFromUnknownGeneration:
        return "completion_from_unknown_generation";
    case RenderLossRefusal::CompletionWhileFenced:
        return "completion_while_fenced";
    case RenderLossRefusal::CompletionFromRetiredOwner:
        return "completion_from_retired_owner";
    case RenderLossRefusal::CallbackAfterTeardown:
        return "callback_after_teardown";
    case RenderLossRefusal::HandoffDuringPendingReplay:
        return "handoff_during_pending_replay";
    case RenderLossRefusal::CompletionBeforeFirstEvent:
        return "completion_before_first_event";
    case RenderLossRefusal::CompletionWithoutGeneration:
        return "completion_without_generation";
    case RenderLossRefusal::CompletionAfterCursorClosed:
        return "completion_after_cursor_closed";
    }
    return "invalid";
}

enum class RenderLossOutcome : std::uint8_t {
    Accepted,
    // H2: exact duplicate of the last applied event. No state change.
    IgnoredDuplicate,
    // H3: an older sequence. Recorded and ignored. No state change.
    IgnoredStale,
    // Refused with the cursor unchanged.
    Rejected,
    // Refused and the cursor closed until an explicit H9 handoff.
    RejectedFailClosed,
};

struct RenderLossVerdict {
    RenderLossOutcome outcome = RenderLossOutcome::Rejected;
    RenderLossRefusal refusal = RenderLossRefusal::None;

    constexpr bool accepted() const noexcept {
        return outcome == RenderLossOutcome::Accepted;
    }
};

enum class RenderLossAdmission : std::uint8_t {
    // No unresolved loss: GPU work that depends on the owner may be admitted.
    Open,
    // A recoverable loss is unresolved: CPU fallback only, until a replay
    // result reports success. Silence never reopens admission.
    Fenced,
    // Permanently closed for this owner. Only H9 installs a new owner.
    Closed,
};

enum class RenderLossCursorClosure : std::uint8_t {
    Open,
    RetiredByConsumer,
    Terminal,
    ResyncRequired,
};

// Kept apart from GpuAudioStatus so the public status struct does not couple
// to a contract that has no producer yet.
struct RenderLossStatus {
    bool owner_installed = false;
    RenderLossAdmission admission = RenderLossAdmission::Closed;
    RenderLossCursorClosure closure = RenderLossCursorClosure::Open;
    bool replay_pending = false;
    // Section 8: true once the most recent loss reported quiescent with
    // callbacks suppressed. Only then may Pulp release resources it shared
    // with the lost generation.
    bool lost_generation_quiescent = false;
    SharedIoFallbackReason loss_fallback_reason = SharedIoFallbackReason::None;
    std::uint64_t last_owner_generation = 0;
    std::uint64_t last_surface_epoch = 0;
    std::uint64_t last_event_seq = 0;
    std::uint64_t accepted_events = 0;
    std::uint64_t duplicates_ignored = 0;
    std::uint64_t stale_ignored = 0;
    std::uint64_t rejected = 0;
    std::uint64_t fail_closed = 0;
    std::uint64_t fence_transitions = 0;
    std::uint64_t reopen_transitions = 0;
    std::uint64_t completions_suppressed = 0;
    std::uint64_t handoffs = 0;
    RenderLossRefusal last_refusal = RenderLossRefusal::None;
};

// Every reason maps to DeviceLost except owner teardown, which maps to
// Teardown (package section 3).
constexpr SharedIoFallbackReason fallback_reason_for(RenderOwnerLossReason reason) noexcept {
    return reason == RenderOwnerLossReason::OwnerTeardown ? SharedIoFallbackReason::Teardown
                                                          : SharedIoFallbackReason::DeviceLost;
}

class RenderLossConsumer {
  public:
    // Retired owner ids are remembered so a retired owner is refused with a
    // precise reason and can never be reinstalled, under any provider (section
    // 3 and H11 key on owner_id). A handoff that would exceed this bound is
    // refused rather than forgetting an older owner.
    static constexpr std::size_t kRetiredOwnerCapacity = 16;
    // Event ids of the most recent resolved losses (terminal, or answered by a
    // replay result). A loss or replay result reusing one fails closed. The
    // memory is a ring: an id older than the last kResolvedEventIdCapacity
    // resolved losses is no longer recognised as reused.
    static constexpr std::size_t kResolvedEventIdCapacity = 32;

    RenderLossConsumer() noexcept = default;
    RenderLossConsumer(const RenderLossConsumer&) = delete;
    RenderLossConsumer& operator=(const RenderLossConsumer&) = delete;
    RenderLossConsumer(RenderLossConsumer&&) = delete;
    RenderLossConsumer& operator=(RenderLossConsumer&&) = delete;

    // Binds the first owner. A second call is an implicit reset and is refused
    // (H10); a different owner arrives only through handoff().
    RenderLossVerdict install(const RenderOwnerIdentity& identity) noexcept {
        if (installed_)
            return refuse(RenderLossRefusal::ImplicitResetForbidden);
        if (!identity.valid())
            return refuse(RenderLossRefusal::MalformedEvent);
        const auto ticket = next_ticket();
        if (ticket == 0)
            return refuse(RenderLossRefusal::AdmissionTicketsExhausted);
        installed_ = true;
        cursor_ = Cursor{};
        cursor_.identity = identity;
        admission_ = RenderLossAdmission::Open;
        admission_ticket_.store(ticket, std::memory_order_release);
        return {RenderLossOutcome::Accepted, RenderLossRefusal::None};
    }

    RenderLossVerdict apply(const RenderOwnerLossEvent& event) noexcept {
        if (event.contract_version != kRenderOwnerLossContractVersion)
            return refuse(RenderLossRefusal::UnknownContractVersion);
        if (!enums_in_domain(event))
            return refuse(RenderLossRefusal::InvalidEnumValue);
        if (!installed_)
            return refuse(RenderLossRefusal::NoOwnerInstalled);
        if (!well_formed(event))
            return refuse(RenderLossRefusal::MalformedEvent);

        if (event.identity != cursor_.identity) {
            if (retired(event.identity.owner_id))
                return refuse(RenderLossRefusal::OwnerReuseWithoutExplicitHandoff);
            if (event.kind == RenderOwnerLossEventKind::ReplayResult && cursor_.replay_pending)
                return refuse(RenderLossRefusal::ReplayIdentityMismatch);
            return refuse(identity_refusal(event.identity));
        }
        if (cursor_.closure != RenderLossCursorClosure::Open)
            return refuse(closed_refusal());

        // H2/H3/H5/H6: sequence position is decided before anything else so
        // a retransmit never reaches the payload rules.
        if (event.event_seq < cursor_.last_event_seq) {
            ++status_.stale_ignored;
            status_.last_refusal = RenderLossRefusal::StaleSequence;
            return {RenderLossOutcome::IgnoredStale, RenderLossRefusal::StaleSequence};
        }
        if (event.event_seq == cursor_.last_event_seq) {
            if (event == cursor_.last_event) {
                ++status_.duplicates_ignored;
                return {RenderLossOutcome::IgnoredDuplicate, RenderLossRefusal::None};
            }
            return fail_closed(RenderLossRefusal::ConflictingPayloadForSequence);
        }
        if (event.event_seq - cursor_.last_event_seq != 1)
            return fail_closed(RenderLossRefusal::SequenceGap);

        // R-03 and section 3 (one event id per loss) are decided before H4:
        // a genuine retransmit of a resolved loss carries the lost generation,
        // which H4 would only refuse as stale without closing the cursor.
        if (event_id_resolved(event.event_id)) {
            return fail_closed(event.kind == RenderOwnerLossEventKind::ReplayResult
                                   ? RenderLossRefusal::DuplicateReplayResult
                                   : RenderLossRefusal::DuplicateLossEventId);
        }
        if (event.kind == RenderOwnerLossEventKind::ReplayResult && !cursor_.replay_pending)
            return fail_closed(RenderLossRefusal::ReplayWithoutLoss);

        // H4. Surface epochs never reset (section 3), so a lower epoch is
        // stale even when the generation moved forward.
        if (event.owner_generation < cursor_.last_owner_generation)
            return refuse(RenderLossRefusal::StaleGeneration);
        if (event.surface_epoch < cursor_.last_surface_epoch)
            return refuse(RenderLossRefusal::StaleSurfaceEpoch);

        if (const auto refusal = shape_refusal(event); refusal != RenderLossRefusal::None)
            return refuse(refusal);

        return event.kind == RenderOwnerLossEventKind::Loss ? apply_loss(event)
                                                            : apply_replay_result(event);
    }

    // H9. Atomically replaces the cursor; the outgoing owner id is retired.
    // Refused while a loss awaits its replay result: only a replay success
    // for that loss lifts its fence. A closed cursor (close(), a terminal
    // event, or a fail-closed refusal) has no pending replay, so a handoff
    // from it installs the new owner with admission open: a fresh owner on a
    // fresh device has no loss to replay. Anything the old owner sends later,
    // including the replay result it never delivered, is refused as a
    // retired owner.
    RenderLossVerdict handoff(const RenderOwnerHandoff& transaction) noexcept {
        if (transaction.contract_version != kRenderOwnerLossContractVersion)
            return refuse(RenderLossRefusal::UnknownContractVersion);
        if (!installed_)
            return refuse(RenderLossRefusal::NoOwnerInstalled);
        if (!transaction.from.valid() || !transaction.to.valid())
            return refuse(RenderLossRefusal::MalformedEvent);
        if (transaction.from != cursor_.identity)
            return refuse(RenderLossRefusal::HandoffFromMismatch);
        if (cursor_.replay_pending)
            return refuse(RenderLossRefusal::HandoffDuringPendingReplay);
        if (transaction.to.owner_id == transaction.from.owner_id ||
            retired(transaction.to.owner_id))
            return refuse(RenderLossRefusal::HandoffToRetiredOwner);
        if (retired_count_ == kRetiredOwnerCapacity)
            return refuse(RenderLossRefusal::HandoffCapacityExhausted);
        const auto ticket = next_ticket();
        if (ticket == 0)
            return refuse(RenderLossRefusal::AdmissionTicketsExhausted);

        retired_owners_[retired_count_++] = cursor_.identity.owner_id;
        Cursor next;
        next.identity = transaction.to;
        cursor_ = next;
        if (admission_ != RenderLossAdmission::Open)
            ++status_.reopen_transitions;
        admission_ = RenderLossAdmission::Open;
        admission_ticket_.store(ticket, std::memory_order_release);
        ++status_.handoffs;
        return {RenderLossOutcome::Accepted, RenderLossRefusal::None};
    }

    // Consumer-initiated retirement of the current owner. Events from it are
    // then refused as owner reuse until a handoff installs a new owner. Any
    // pending replay is abandoned with the owner; admission stays closed.
    void close() noexcept {
        if (!installed_ || cursor_.closure != RenderLossCursorClosure::Open)
            return;
        cursor_.closure = RenderLossCursorClosure::RetiredByConsumer;
        cursor_.replay_pending = false;
        set_admission(RenderLossAdmission::Closed);
    }

    // H12. Decides whether a GPU completion tagged with an owner generation
    // may retire audio work. Anything but Accepted must be suppressed.
    RenderLossVerdict classify_completion(const RenderOwnerIdentity& identity,
                                          std::uint64_t owner_generation) noexcept {
        auto verdict = classify(identity, owner_generation);
        if (!verdict.accepted()) {
            ++status_.completions_suppressed;
            status_.last_refusal = verdict.refusal;
        }
        return verdict;
    }

    // Realtime-safe. Zero means GPU admission is fenced or closed. A ticket
    // captured at admission retires only while it is still current, so a
    // fence or reopen between admission and completion always invalidates it.
    std::uint64_t admission_ticket() const noexcept {
        return admission_ticket_.load(std::memory_order_acquire);
    }
    bool admission_ticket_current(std::uint64_t ticket) const noexcept {
        return ticket != 0 && ticket == admission_ticket_.load(std::memory_order_acquire);
    }

    const RenderOwnerIdentity& identity() const noexcept {
        return cursor_.identity;
    }
    RenderLossAdmission admission() const noexcept {
        return admission_;
    }
    RenderLossCursorClosure closure() const noexcept {
        return cursor_.closure;
    }
    std::uint64_t last_event_seq() const noexcept {
        return cursor_.last_event_seq;
    }
    std::uint64_t last_owner_generation() const noexcept {
        return cursor_.last_owner_generation;
    }
    std::uint64_t last_surface_epoch() const noexcept {
        return cursor_.last_surface_epoch;
    }

    RenderLossStatus status() const noexcept {
        auto out = status_;
        out.owner_installed = installed_;
        out.admission = admission_;
        out.closure = cursor_.closure;
        out.replay_pending = cursor_.replay_pending;
        out.lost_generation_quiescent = cursor_.lost_generation_quiescent;
        out.loss_fallback_reason = cursor_.loss_fallback_reason;
        out.last_owner_generation = cursor_.last_owner_generation;
        out.last_surface_epoch = cursor_.last_surface_epoch;
        out.last_event_seq = cursor_.last_event_seq;
        return out;
    }

  private:
    struct Cursor {
        RenderOwnerIdentity identity;
        std::uint64_t last_owner_generation = 0;
        std::uint64_t last_surface_epoch = 0;
        std::uint64_t last_event_seq = 0;
        RenderOwnerLossEvent last_event;
        RenderLossCursorClosure closure = RenderLossCursorClosure::Open;
        // The loss awaiting its replay result, frozen at loss publication.
        bool replay_pending = false;
        RenderLossText pending_event_id;
        std::uint64_t pending_generation = 0;
        std::uint64_t pending_surface_epoch = 0;
        RenderResourceManifest pending_manifest;
        RenderOwnerLossReason pending_reason = RenderOwnerLossReason::Unknown;
        RenderOwnerQuiescence episode_quiescence = RenderOwnerQuiescence::Active;
        bool lost_generation_quiescent = false;
        SharedIoFallbackReason loss_fallback_reason = SharedIoFallbackReason::None;
    };

    RenderLossVerdict apply_loss(const RenderOwnerLossEvent& event) noexcept {
        if (cursor_.replay_pending) {
            if (event.quiescence < cursor_.episode_quiescence)
                return fail_closed(RenderLossRefusal::QuiescenceRegression);
            // R-08 is open for Vellum; the package's proposed rule refuses a
            // second open loss on one owner.
            return fail_closed(event.event_id == cursor_.pending_event_id
                                   ? RenderLossRefusal::DuplicateLossEventId
                                   : RenderLossRefusal::InterleavedLossDuringReplay);
        }

        // H1: build the whole next cursor, then commit it in one assignment.
        Cursor next = cursor_;
        advance(next, event);
        next.last_owner_generation = event.owner_generation;
        next.last_surface_epoch = event.surface_epoch;
        next.lost_generation_quiescent = event.quiescence == RenderOwnerQuiescence::Quiescent;
        next.loss_fallback_reason = fallback_reason_for(event.reason);
        auto admission = RenderLossAdmission::Fenced;
        if (event.disposition == RenderOwnerLossDisposition::Terminal) {
            next.closure = RenderLossCursorClosure::Terminal;
            admission = RenderLossAdmission::Closed;
        } else {
            next.replay_pending = true;
            next.pending_event_id = event.event_id;
            next.pending_generation = event.owner_generation;
            next.pending_surface_epoch = event.surface_epoch;
            next.pending_manifest = event.resource_manifest;
            next.pending_reason = event.reason;
            next.episode_quiescence = event.quiescence;
        }
        cursor_ = next;
        if (event.disposition == RenderOwnerLossDisposition::Terminal)
            remember_resolved(event.event_id);
        set_admission(admission);
        ++status_.accepted_events;
        return {RenderLossOutcome::Accepted, RenderLossRefusal::None};
    }

    RenderLossVerdict apply_replay_result(const RenderOwnerLossEvent& event) noexcept {
        if (!cursor_.replay_pending)
            return fail_closed(RenderLossRefusal::ReplayWithoutLoss);
        if (event.event_id != cursor_.pending_event_id)
            return fail_closed(RenderLossRefusal::ReplayEventIdMismatch);
        if (event.owner_generation != cursor_.pending_generation ||
            event.surface_epoch != cursor_.pending_surface_epoch)
            return refuse(RenderLossRefusal::ReplayGenerationMismatch);
        if (event.resource_manifest.schema_version != cursor_.pending_manifest.schema_version)
            return refuse(RenderLossRefusal::ResourceManifestIdentityChanged);
        if (event.resource_manifest.digest_sha256 != cursor_.pending_manifest.digest_sha256)
            return refuse(RenderLossRefusal::ResourceManifestDigestChanged);
        if (event.quiescence < cursor_.episode_quiescence)
            return fail_closed(RenderLossRefusal::QuiescenceRegression);

        const bool succeeded = event.replay == RenderOwnerReplay::Succeeded;
        std::uint64_t ticket = 0;
        if (succeeded) {
            if (event.next_owner_generation <= event.owner_generation ||
                event.next_surface_epoch <= event.surface_epoch)
                return refuse(RenderLossRefusal::ReplaySuccessRequiresNewGenerationAndSurfaceEpoch);
            if (cursor_.pending_reason == RenderOwnerLossReason::Unknown)
                return refuse(RenderLossRefusal::ReplaySuccessRequiresKnownReason);
            ticket = next_ticket();
            if (ticket == 0)
                return fail_closed(RenderLossRefusal::AdmissionTicketsExhausted);
        }

        Cursor next = cursor_;
        advance(next, event);
        next.replay_pending = false;
        if (event.quiescence == RenderOwnerQuiescence::Quiescent)
            next.lost_generation_quiescent = true;
        if (succeeded) {
            next.last_owner_generation = event.next_owner_generation;
            next.last_surface_epoch = event.next_surface_epoch;
        } else {
            next.closure = RenderLossCursorClosure::Terminal;
        }
        cursor_ = next;
        remember_resolved(event.event_id);
        if (succeeded) {
            ++status_.reopen_transitions;
            admission_ = RenderLossAdmission::Open;
            admission_ticket_.store(ticket, std::memory_order_release);
        } else {
            set_admission(RenderLossAdmission::Closed);
        }
        ++status_.accepted_events;
        return {RenderLossOutcome::Accepted, RenderLossRefusal::None};
    }

    // Field-level rules that need no cursor state beyond identity and order.
    static RenderLossRefusal shape_refusal(const RenderOwnerLossEvent& event) noexcept {
        const bool terminal = event.disposition == RenderOwnerLossDisposition::Terminal;
        if (event.quiescence == RenderOwnerQuiescence::Quiescent && !event.callbacks_suppressed)
            return RenderLossRefusal::QuiescentRequiresCallbacksSuppressed;
        if (terminal) {
            if (!event.has_terminal)
                return RenderLossRefusal::InvalidTerminalBlock;
            if (event.quiescence != RenderOwnerQuiescence::Quiescent ||
                !event.callbacks_suppressed || !event.terminal.quiescent ||
                !event.terminal.callbacks_suppressed)
                return RenderLossRefusal::TerminalRequiresQuiescentAndCallbacksSuppressed;
            if (event.replay == RenderOwnerReplay::Succeeded ||
                event.terminal.replay != event.replay || event.next_owner_generation != 0 ||
                event.next_surface_epoch != 0)
                return RenderLossRefusal::InvalidTerminalBlock;
        } else if (event.has_terminal) {
            return RenderLossRefusal::InvalidTerminalBlock;
        }
        const bool teardown_reason = event.reason == RenderOwnerLossReason::OwnerTeardown;
        const bool teardown_kind =
            terminal && event.terminal.kind == RenderOwnerTerminalKind::OwnerTeardown;
        if (teardown_reason != teardown_kind)
            return RenderLossRefusal::ReasonTerminalKindMismatch;

        if (event.kind == RenderOwnerLossEventKind::Loss) {
            if (event.replay != RenderOwnerReplay::NotAttempted ||
                event.next_owner_generation != 0 || event.next_surface_epoch != 0)
                return RenderLossRefusal::InvalidLossReplayField;
            if (terminal && event.terminal.kind == RenderOwnerTerminalKind::ReplayFailed)
                return RenderLossRefusal::InvalidTerminalBlock;
            return RenderLossRefusal::None;
        }
        switch (event.replay) {
        case RenderOwnerReplay::NotAttempted:
            return RenderLossRefusal::InvalidReplayOutcome;
        case RenderOwnerReplay::Succeeded:
            return RenderLossRefusal::None;
        case RenderOwnerReplay::Failed:
            if (!terminal)
                return RenderLossRefusal::InvalidReplayOutcome;
            if (event.terminal.kind != RenderOwnerTerminalKind::ReplayFailed &&
                event.terminal.kind != RenderOwnerTerminalKind::DeviceLost)
                return RenderLossRefusal::InvalidTerminalBlock;
            return RenderLossRefusal::None;
        }
        return RenderLossRefusal::InvalidEnumValue;
    }

    RenderLossVerdict classify(const RenderOwnerIdentity& identity,
                               std::uint64_t owner_generation) const noexcept {
        auto suppressed = [](RenderLossRefusal refusal) {
            return RenderLossVerdict{RenderLossOutcome::Rejected, refusal};
        };
        if (!installed_)
            return suppressed(RenderLossRefusal::NoOwnerInstalled);
        if (identity != cursor_.identity) {
            return suppressed(retired(identity.owner_id)
                                  ? RenderLossRefusal::CompletionFromRetiredOwner
                                  : identity_refusal(identity));
        }
        switch (cursor_.closure) {
        case RenderLossCursorClosure::Open:
            break;
        case RenderLossCursorClosure::Terminal:
            return suppressed(RenderLossRefusal::CallbackAfterTeardown);
        case RenderLossCursorClosure::RetiredByConsumer:
        case RenderLossCursorClosure::ResyncRequired:
            return suppressed(RenderLossRefusal::CompletionAfterCursorClosed);
        }
        if (owner_generation == 0)
            return suppressed(RenderLossRefusal::CompletionWithoutGeneration);
        // Before the owner's first event the cursor has no generation floor,
        // so nothing can be classified; the admission ticket alone gates the
        // healthy path. After it, an unannounced generation cannot retire.
        if (cursor_.last_event_seq == 0)
            return suppressed(RenderLossRefusal::CompletionBeforeFirstEvent);
        if (owner_generation < cursor_.last_owner_generation)
            return suppressed(RenderLossRefusal::CompletionFromLostGeneration);
        if (owner_generation > cursor_.last_owner_generation)
            return suppressed(RenderLossRefusal::CompletionFromUnknownGeneration);
        if (admission_ != RenderLossAdmission::Open)
            return suppressed(RenderLossRefusal::CompletionWhileFenced);
        return {RenderLossOutcome::Accepted, RenderLossRefusal::None};
    }

    static bool enums_in_domain(const RenderOwnerLossEvent& event) noexcept {
        return static_cast<std::uint8_t>(event.kind) <=
                   static_cast<std::uint8_t>(RenderOwnerLossEventKind::ReplayResult) &&
               static_cast<std::uint8_t>(event.reason) <=
                   static_cast<std::uint8_t>(RenderOwnerLossReason::Unknown) &&
               static_cast<std::uint8_t>(event.disposition) <=
                   static_cast<std::uint8_t>(RenderOwnerLossDisposition::Terminal) &&
               static_cast<std::uint8_t>(event.quiescence) <=
                   static_cast<std::uint8_t>(RenderOwnerQuiescence::Quiescent) &&
               static_cast<std::uint8_t>(event.replay) <=
                   static_cast<std::uint8_t>(RenderOwnerReplay::Failed) &&
               static_cast<std::uint8_t>(event.terminal.kind) <=
                   static_cast<std::uint8_t>(RenderOwnerTerminalKind::OwnerTeardown) &&
               static_cast<std::uint8_t>(event.terminal.replay) <=
                   static_cast<std::uint8_t>(RenderOwnerReplay::Failed);
    }

    // Identity, event id, manifest identity and payload digest are required
    // on both event kinds; generations, epochs and sequences start at 1.
    static bool well_formed(const RenderOwnerLossEvent& event) noexcept {
        constexpr RenderLossDigest absent{};
        return event.identity.valid() && event.event_id.valid() && event.event_seq != 0 &&
               event.owner_generation != 0 && event.surface_epoch != 0 &&
               event.resource_manifest.schema_version.valid() &&
               event.resource_manifest.digest_sha256 != absent &&
               event.payload_digest_sha256 != absent;
    }

    RenderLossRefusal identity_refusal(const RenderOwnerIdentity& identity) const noexcept {
        return identity.provider_id != cursor_.identity.provider_id ||
                       identity.provider_version != cursor_.identity.provider_version
                   ? RenderLossRefusal::UnexpectedProviderIdentity
                   : RenderLossRefusal::UnexpectedOwnerIdentity;
    }

    RenderLossRefusal closed_refusal() const noexcept {
        switch (cursor_.closure) {
        case RenderLossCursorClosure::Terminal:
            return RenderLossRefusal::EventAfterTerminal;
        case RenderLossCursorClosure::ResyncRequired:
            return RenderLossRefusal::CursorResyncRequired;
        case RenderLossCursorClosure::RetiredByConsumer:
        case RenderLossCursorClosure::Open:
            break;
        }
        return RenderLossRefusal::OwnerReuseWithoutExplicitHandoff;
    }

    bool retired(const RenderLossText& owner_id) const noexcept {
        for (std::size_t i = 0; i < retired_count_; ++i)
            if (retired_owners_[i] == owner_id)
                return true;
        return false;
    }

    bool event_id_resolved(const RenderLossText& event_id) const noexcept {
        const auto count =
            resolved_count_ < kResolvedEventIdCapacity ? resolved_count_ : kResolvedEventIdCapacity;
        for (std::size_t i = 0; i < count; ++i)
            if (resolved_event_ids_[i] == event_id)
                return true;
        return false;
    }

    void remember_resolved(const RenderLossText& event_id) noexcept {
        resolved_event_ids_[resolved_count_ % kResolvedEventIdCapacity] = event_id;
        ++resolved_count_;
    }

    static void advance(Cursor& next, const RenderOwnerLossEvent& event) noexcept {
        next.last_event_seq = event.event_seq;
        next.last_event = event;
    }

    std::uint64_t next_ticket() noexcept {
        if (ticket_serial_ == std::numeric_limits<std::uint64_t>::max())
            return 0;
        return ++ticket_serial_;
    }

    void set_admission(RenderLossAdmission admission) noexcept {
        if (admission_ == RenderLossAdmission::Open && admission != RenderLossAdmission::Open)
            ++status_.fence_transitions;
        admission_ = admission;
        admission_ticket_.store(0, std::memory_order_release);
    }

    RenderLossVerdict refuse(RenderLossRefusal refusal) noexcept {
        ++status_.rejected;
        status_.last_refusal = refusal;
        return {RenderLossOutcome::Rejected, refusal};
    }

    // H5/H6: neither the offending event nor anything after it is applied
    // until an explicit handoff installs a fresh cursor.
    RenderLossVerdict fail_closed(RenderLossRefusal refusal) noexcept {
        ++status_.rejected;
        ++status_.fail_closed;
        status_.last_refusal = refusal;
        cursor_.closure = RenderLossCursorClosure::ResyncRequired;
        cursor_.replay_pending = false;
        set_admission(RenderLossAdmission::Closed);
        return {RenderLossOutcome::RejectedFailClosed, refusal};
    }

    bool installed_ = false;
    Cursor cursor_;
    RenderLossAdmission admission_ = RenderLossAdmission::Closed;
    std::atomic<std::uint64_t> admission_ticket_{0};
    std::uint64_t ticket_serial_ = 0;
    std::array<RenderLossText, kRetiredOwnerCapacity> retired_owners_{};
    std::size_t retired_count_ = 0;
    // Kept across handoffs: an event id is never reused by any owner.
    std::array<RenderLossText, kResolvedEventIdCapacity> resolved_event_ids_{};
    std::size_t resolved_count_ = 0;
    RenderLossStatus status_;
};

} // namespace pulp::gpu_audio::detail
