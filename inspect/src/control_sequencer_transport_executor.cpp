#include <pulp/inspect/control_sequencer_transport_executor.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timeline_editor/loop_range.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::int64_t kMaximumWireInteger = 9007199254740991;

ControlExecutionOutcome fail(ControlResultCode code, std::string explanation,
                             ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)}};
}

ControlExecutionOutcome cancelled(std::string explanation) {
    return {.terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = explanation,
                       .cancellation_reason = std::move(explanation)}};
}

/// Maps a checkpoint that is not Continue onto its terminal outcome.
ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint,
                                           std::string_view stage) {
    if (checkpoint == ControlExecutionCheckpoint::Cancelled)
        return cancelled(std::string{"transport loop "} + std::string{stage} + " was cancelled");
    if (checkpoint == ControlExecutionCheckpoint::AuthorityRevoked)
        return cancelled(std::string{"transport loop "} + std::string{stage} +
                         " authority was revoked");
    return fail(ControlResultCode::DeadlineExceeded,
                std::string{"transport loop "} + std::string{stage} + " deadline elapsed");
}

/// Every refusal MasterTransport can return, each with its own reachable
/// message. The transport is the authority on whether a loop was accepted, so
/// its verdict is reported verbatim rather than being folded into one generic
/// rejection or replaced by the tick-domain helper's answer.
std::string_view transport_refusal(playback::TransportError error) {
    switch (error) {
    case playback::TransportError::NotPrepared:
        return "the transport has not been prepared, so it holds no sample mapping for a loop";
    case playback::TransportError::InvalidLoop:
        return "the transport rejected the loop: its endpoints do not span a positive sample range";
    case playback::TransportError::LoopTooShortForMaximumBlock:
        return "the transport rejected the loop: its sample span is shorter than the prepared "
               "maximum block";
    case playback::TransportError::InvalidMeter:
        return "the transport rejected the loop with an invalid meter";
    case playback::TransportError::InvalidFrameCount:
        return "the transport rejected the loop with an invalid frame count";
    case playback::TransportError::InvalidScrubWindow:
        return "the transport rejected the loop with an invalid scrub window";
    case playback::TransportError::ScrubWindowTooShortForMaximumBlock:
        return "the transport rejected the loop with a scrub window shorter than the maximum block";
    case playback::TransportError::NotScrubbing:
        return "the transport rejected the loop because it is not scrubbing";
    case playback::TransportError::InvalidTempo:
        return "the transport rejected the loop with an invalid tempo";
    case playback::TransportError::InvalidTempoSyncConfig:
        return "the transport rejected the loop with an invalid tempo-sync configuration";
    case playback::TransportError::TempoSyncHostTimeRequired:
        return "the transport rejected the loop because tempo sync requires host time";
    case playback::TransportError::TempoSyncUnavailable:
        return "the transport rejected the loop because tempo sync is unavailable";
    case playback::TransportError::InvalidTempoSyncState:
        return "the transport rejected the loop with an invalid tempo-sync state";
    case playback::TransportError::PlaybackEpochExhausted:
        return "the transport rejected the loop because its playback epoch is exhausted";
    case playback::TransportError::None:
        break;
    }
    return "the transport accepted the loop";
}

std::string_view loop_range_refusal(timeline_editor::LoopRangeError error) {
    switch (error) {
    case timeline_editor::LoopRangeError::CollapsedSpan:
        break;
    }
    return "the submitted endpoints resolve to the same document tick";
}

/// Document ticks are signed: the canonical tick for sample zero on a compiled
/// tempo map is itself negative, because `samples_to_ticks` returns the first
/// tick that rounds onto that sample. The wire therefore carries the symmetric
/// JSON-safe integer range, and a bound outside it is refused rather than
/// silently truncated.
bool tick_is_on_the_wire(std::int64_t tick) {
    return tick >= -kMaximumWireInteger && tick <= kMaximumWireInteger;
}

std::optional<ControlSequencerTransportTarget>
resolve_live_target(const ControlSequencerTransportTargetResolver& resolve_target,
                    const ControlAdmissionPlan& plan) {
    auto target = resolve_target(plan);
    if (!target || target->transport == nullptr ||
        target->registration_id != plan.registration_id ||
        (target->host_tier != ControlHostTier::Standalone &&
         target->host_tier != ControlHostTier::SharedPluginHost))
        return std::nullopt;
    return target;
}

/// MasterTransport::set_loop publishes nothing: it records the desired loop and
/// the audio thread republishes the playhead at its next block. So between an
/// accepted edit and the next block the published playhead still carries the
/// previous loop — permanently so while the transport is stopped. Remembering
/// the loop the transport last accepted is what keeps set-enabled from toggling
/// a superseded range back over an accepted edit. It is not an invented loop:
/// set_loop returning None is the transport's own acceptance.
struct AcceptedLoopRecord {
    std::optional<timebase::LoopRegion> loop;
    std::uint64_t publication_sequence = 0;
};

} // namespace

ControlOperationExecutor make_control_sequencer_transport_read_executor(
    ControlSequencerTransportTargetResolver resolve_target) {
    return [resolve_target = std::move(resolve_target)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.sequencer/transport.loop.read@1" ||
            request.operation_version != 1 || !resolve_target || !context.checkpoint) {
            return fail(ControlResultCode::InvalidRequest,
                        "transport loop read executor is unavailable for this operation");
        }
        const auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint, "read");
        if (events::MainThreadDispatcher::has_backend() &&
            !events::MainThreadDispatcher::is_main_thread()) {
            return fail(ControlResultCode::HostUnavailable,
                        "transport loop read requires the host main thread",
                        ControlRetryClassification::AfterBackoff);
        }

        const auto target = resolve_live_target(resolve_target, plan);
        if (!target)
            return fail(ControlResultCode::HostUnavailable,
                        "no live transport is bound to this registration",
                        ControlRetryClassification::AfterRefresh);

        const auto playhead = target->transport->playhead();
        if (playhead.sequence == 0)
            return fail(ControlResultCode::HostUnavailable,
                        "the transport has published no state, so it holds no loop to report",
                        ControlRetryClassification::AfterRefresh);
        if (playhead.sequence > static_cast<std::uint64_t>(kMaximumWireInteger))
            return fail(ControlResultCode::ResourceExhausted,
                        "the transport publication sequence exceeds the canonical wire range");
        if (!tick_is_on_the_wire(playhead.loop.start.value) ||
            !tick_is_on_the_wire(playhead.loop.end.value))
            return fail(ControlResultCode::ResourceExhausted,
                        "the transport loop bounds exceed the canonical wire range");

        auto detail = choc::value::createObject("ControlSequencerTransportLoopReadResult");
        detail.setMember("sequence", static_cast<std::int64_t>(playhead.sequence));
        detail.setMember("enabled", playhead.loop.enabled);
        detail.setMember("start_tick", playhead.loop.start.value);
        detail.setMember("end_tick", playhead.loop.end.value);
        detail.setMember("playing", playhead.is_playing);
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    };
}

ControlOperationExecutor make_control_sequencer_transport_write_executor(
    ControlSequencerTransportTargetResolver resolve_target) {
    return [resolve_target = std::move(resolve_target),
            accepted = std::make_shared<AcceptedLoopRecord>()](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.sequencer/transport.loop.write@1" ||
            request.operation_version != 1 ||
            request.registration_id != plan.registration_id.value || !resolve_target ||
            !context.checkpoint) {
            return fail(ControlResultCode::InvalidRequest,
                        "transport loop write executor is unavailable for this operation");
        }

        bool set_enabled_action = false;
        bool requested_enabled = false;
        std::int64_t requested_sequence = 0;
        std::int64_t first_tick = 0;
        std::int64_t second_tick = 0;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject() || !params.hasObjectMember("action") ||
                !params["action"].isString())
                return fail(ControlResultCode::InvalidRequest,
                            "transport loop write request was not canonical");
            const auto action = params["action"].getString();
            if (action == "set-range") {
                if (!params.hasObjectMember("start_tick") || !params["start_tick"].isInt() ||
                    !params.hasObjectMember("end_tick") || !params["end_tick"].isInt())
                    return fail(ControlResultCode::InvalidRequest,
                                "set-range requires both already-snapped endpoints");
                first_tick = params["start_tick"].getInt64();
                second_tick = params["end_tick"].getInt64();
                if (!tick_is_on_the_wire(first_tick) || !tick_is_on_the_wire(second_tick))
                    return fail(ControlResultCode::InvalidRequest,
                                "set-range endpoints exceeded the canonical wire range");
            } else if (action == "set-enabled") {
                set_enabled_action = true;
                if (params.hasObjectMember("start_tick") || params.hasObjectMember("end_tick"))
                    return fail(ControlResultCode::InvalidRequest,
                                "set-enabled carries no endpoints; it reuses the transport's own "
                                "published bounds");
                if (!params.hasObjectMember("enabled") || !params["enabled"].isBool() ||
                    !params.hasObjectMember("expected_sequence") ||
                    !params["expected_sequence"].isInt())
                    return fail(ControlResultCode::InvalidRequest,
                                "set-enabled requires an enabled flag and the publication "
                                "sequence its bounds were read from");
                requested_enabled = params["enabled"].getBool();
                requested_sequence = params["expected_sequence"].getInt64();
                if (requested_sequence < 1 || !tick_is_on_the_wire(requested_sequence))
                    return fail(ControlResultCode::InvalidRequest,
                                "expected_sequence exceeded schema bounds");
            } else {
                return fail(ControlResultCode::InvalidRequest,
                            "transport loop write action is not a known branch");
            }
        } catch (...) {
            return fail(ControlResultCode::InvalidRequest,
                        "transport loop write request could not be decoded");
        }

        const auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint, "write");
        if (events::MainThreadDispatcher::has_backend() &&
            !events::MainThreadDispatcher::is_main_thread()) {
            return fail(ControlResultCode::HostUnavailable,
                        "transport loop write requires the host main thread",
                        ControlRetryClassification::AfterBackoff);
        }

        const auto target = resolve_live_target(resolve_target, plan);
        if (!target)
            return fail(ControlResultCode::HostUnavailable,
                        "no live transport is bound to this registration",
                        ControlRetryClassification::AfterRefresh);

        timebase::LoopRegion candidate{};
        if (set_enabled_action) {
            const auto playhead = target->transport->playhead();
            if (playhead.sequence == 0)
                return fail(ControlResultCode::HostUnavailable,
                            "the transport has published no bounds for set-enabled to reuse",
                            ControlRetryClassification::AfterRefresh);
            if (playhead.sequence != static_cast<std::uint64_t>(requested_sequence))
                return fail(ControlResultCode::StateConflict,
                            "the transport republished its loop after expected_sequence was read",
                            ControlRetryClassification::AfterRefresh);
            const auto base = accepted->loop && accepted->publication_sequence == playhead.sequence
                                  ? *accepted->loop
                                  : playhead.loop;
            candidate = timeline_editor::with_loop_enabled(base, requested_enabled);
        } else {
            const auto range = timeline_editor::loop_region_from_snapped_endpoints(
                timebase::TickPosition{first_tick}, timebase::TickPosition{second_tick});
            if (range.is_err())
                return fail(ControlResultCode::InvalidRequest,
                            std::string{loop_range_refusal(range.error())});
            candidate = range.value();
        }

        const auto apply_checkpoint = context.checkpoint();
        if (apply_checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(apply_checkpoint, "write apply");

        const auto verdict = target->transport->set_loop(candidate);
        if (verdict != playback::TransportError::None)
            return fail(ControlResultCode::InvalidRequest,
                        std::string{transport_refusal(verdict)});

        const auto published = target->transport->playhead();
        accepted->loop = candidate;
        accepted->publication_sequence = published.sequence;
        auto detail = choc::value::createObject("ControlSequencerTransportLoopWriteResult");
        detail.setMember("receipt_id", plan.receipt_id.value);
        detail.setMember("action", set_enabled_action ? "set-enabled" : "set-range");
        detail.setMember("applied", true);
        detail.setMember("enabled", candidate.enabled);
        detail.setMember("start_tick", candidate.start.value);
        detail.setMember("end_tick", candidate.end.value);
        detail.setMember("sequence",
                         published.sequence > static_cast<std::uint64_t>(kMaximumWireInteger)
                             ? kMaximumWireInteger
                             : static_cast<std::int64_t>(published.sequence));
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    };
}

} // namespace pulp::inspect
