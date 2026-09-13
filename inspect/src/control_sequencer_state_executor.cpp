#include <pulp/inspect/control_sequencer_state_executor.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::inspect {
namespace {

using Config = state::ReferenceSequencerConfig;
constexpr std::uint8_t kLanes = static_cast<std::uint8_t>(Config::lanes);
constexpr std::uint8_t kSteps = static_cast<std::uint8_t>(Config::steps);
constexpr std::uint8_t kPatterns = static_cast<std::uint8_t>(Config::patterns);

ControlExecutionOutcome fail(ControlResultCode code, std::string explanation,
                             ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)}};
}

ControlExecutionOutcome checkpoint_outcome(ControlExecutionCheckpoint checkpoint,
                                           std::string_view subject) {
    if (checkpoint == ControlExecutionCheckpoint::Cancelled ||
        checkpoint == ControlExecutionCheckpoint::AuthorityRevoked) {
        const bool cancelled = checkpoint == ControlExecutionCheckpoint::Cancelled;
        return {.terminal_state = ControlReceiptState::Cancelled,
                .result = {.result_code = ControlResultCode::Cancelled,
                           .explanation = std::string(subject) +
                                          (cancelled ? " cancelled" : " authority revoked"),
                           .cancellation_reason =
                               cancelled ? "client-cancelled" : "authority-revoked"}};
    }
    return fail(ControlResultCode::DeadlineExceeded, std::string(subject) + " deadline exceeded");
}

/// Refuses any call that is not on the host main thread once a real dispatcher
/// backend exists. This is the runtime half of the HostMain executor binding
/// declared in capability_definitions.inc, and it is what keeps the channel's
/// single-writer command FIFO contract intact.
bool off_host_main_thread() {
    return events::MainThreadDispatcher::has_backend() &&
           !events::MainThreadDispatcher::is_main_thread();
}

bool read_bool(const choc::value::ValueView& params, std::string_view name, bool fallback,
               bool& ok) {
    if (!params.hasObjectMember(name)) return fallback;
    const auto member = params[name];
    if (!member.isBool()) {
        ok = false;
        return fallback;
    }
    return member.getBool();
}

/// Reads one closed unsigned integer member. `present` distinguishes an absent
/// member from a supplied zero so required-field validation stays exact.
std::uint64_t read_uint(const choc::value::ValueView& params, std::string_view name,
                        std::uint64_t maximum, bool& ok, bool& present) {
    present = params.hasObjectMember(name);
    if (!present) return 0;
    const auto member = params[name];
    if (!member.isInt()) {
        ok = false;
        return 0;
    }
    const auto raw = member.getInt64();
    if (raw < 0 || static_cast<std::uint64_t>(raw) > maximum) {
        ok = false;
        return 0;
    }
    return static_cast<std::uint64_t>(raw);
}

std::int64_t read_int(const choc::value::ValueView& params, std::string_view name,
                      std::int64_t minimum, std::int64_t maximum, bool& ok, bool& present) {
    present = params.hasObjectMember(name);
    if (!present) return 0;
    const auto member = params[name];
    if (!member.isInt()) {
        ok = false;
        return 0;
    }
    const auto raw = member.getInt64();
    if (raw < minimum || raw > maximum) {
        ok = false;
        return 0;
    }
    return raw;
}

std::string read_string(const choc::value::ValueView& params, std::string_view name, bool& ok) {
    if (!params.hasObjectMember(name)) return {};
    const auto member = params[name];
    if (!member.isString()) {
        ok = false;
        return {};
    }
    return std::string(member.getString());
}

choc::value::Value cell_to_value(const state::StepCell& cell) {
    auto item = choc::value::createObject("SequencerStepCell");
    item.setMember("enabled", cell.enabled());
    item.setMember("velocity", static_cast<std::int64_t>(cell.velocity));
    item.setMember("probability", static_cast<std::int64_t>(cell.probability));
    item.setMember("pitch_offset", static_cast<std::int64_t>(cell.pitch_offset));
    item.setMember("gate_ticks", static_cast<std::int64_t>(cell.gate_ticks));
    item.setMember("ratchet", static_cast<std::int64_t>(cell.ratchet));
    return item;
}

bool value_to_cell(const choc::value::ValueView& value, state::StepCell& cell) {
    if (!value.isObject()) return false;
    bool ok = true;
    bool present = false;
    const auto enabled_present = value.hasObjectMember("enabled");
    if (!enabled_present || !value["enabled"].isBool()) return false;
    const auto velocity = read_uint(value, "velocity", 255, ok, present);
    if (!ok || !present) return false;
    const auto probability = read_uint(value, "probability", 255, ok, present);
    if (!ok || !present) return false;
    const auto pitch = read_int(value, "pitch_offset", -128, 127, ok, present);
    if (!ok || !present) return false;
    const auto gate = read_uint(value, "gate_ticks", 65535, ok, present);
    if (!ok || !present) return false;
    const auto ratchet = read_uint(value, "ratchet", 255, ok, present);
    if (!ok || !present) return false;
    cell = state::StepCell{};
    cell.flags = value["enabled"].getBool() ? state::StepCell::kEnabledBit : std::uint8_t{0};
    cell.velocity = static_cast<std::uint8_t>(velocity);
    cell.probability = static_cast<std::uint8_t>(probability);
    cell.pitch_offset = static_cast<std::int8_t>(pitch);
    cell.gate_ticks = static_cast<std::uint16_t>(gate);
    cell.ratchet = static_cast<std::uint8_t>(ratchet);
    return true;
}

bool gesture_phase_from_id(std::string_view id, state::GesturePhase& phase) {
    if (id == "none") phase = state::GesturePhase::None;
    else if (id == "begin") phase = state::GesturePhase::Begin;
    else if (id == "update") phase = state::GesturePhase::Update;
    else if (id == "end") phase = state::GesturePhase::End;
    else if (id == "cancel") phase = state::GesturePhase::Cancel;
    else return false;
    return true;
}

bool clear_scope_from_id(std::string_view id, state::ClearScope& scope) {
    if (id == "cell") scope = state::ClearScope::Cell;
    else if (id == "lane") scope = state::ClearScope::Lane;
    else if (id == "pattern") scope = state::ClearScope::Pattern;
    else if (id == "all") scope = state::ClearScope::All;
    else return false;
    return true;
}

/// Resolves the admitted channel. Host tiers mirror the parameter-gesture
/// executor: only a Pulp-owned Standalone or shared plugin host may expose a
/// live channel.
std::optional<ControlSequencerStateTarget>
resolve(const ControlSequencerStateTargetResolver& resolve_target,
        const ControlAdmissionPlan& plan) {
    auto target = resolve_target(plan);
    if (!target || !target->channel || target->registration_id != plan.registration_id ||
        (target->host_tier != ControlHostTier::Standalone &&
         target->host_tier != ControlHostTier::SharedPluginHost))
        return std::nullopt;
    return target;
}

} // namespace

ControlOperationExecutor
make_control_sequencer_state_read_executor(ControlSequencerStateTargetResolver resolve_target) {
    return [resolve_target = std::move(resolve_target)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.sequencer/state.read@1" ||
            request.operation_version != 1 ||
            request.registration_id != plan.registration_id.value || !resolve_target ||
            !context.checkpoint) {
            return fail(ControlResultCode::InvalidRequest,
                        "sequencer state read executor is unavailable for this operation");
        }

        bool include_snapshot = true;
        bool include_playhead = true;
        bool pattern_selected = false;
        std::uint8_t selected_pattern = 0;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject())
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer state read request was not canonical");
            bool ok = true;
            include_snapshot = read_bool(params, "include_snapshot", true, ok);
            include_playhead = read_bool(params, "include_playhead", true, ok);
            bool present = false;
            const auto pattern =
                read_uint(params, "pattern", static_cast<std::uint64_t>(kPatterns - 1), ok, present);
            if (!ok)
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer state read request exceeded schema bounds");
            pattern_selected = present;
            selected_pattern = static_cast<std::uint8_t>(pattern);
        } catch (...) {
            return fail(ControlResultCode::InvalidRequest,
                        "sequencer state read request could not be decoded");
        }

        auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_outcome(checkpoint, "sequencer state read");
        if (off_host_main_thread())
            return fail(ControlResultCode::HostUnavailable,
                        "sequencer state read requires the host main thread",
                        ControlRetryClassification::AfterBackoff);

        auto target = resolve(resolve_target, plan);
        if (!target)
            return fail(ControlResultCode::HostUnavailable,
                        "exact sequencer channel is unavailable",
                        ControlRetryClassification::AfterRefresh);

        // LIFETIME: ui_read_latest_snapshot's reference is valid only until the
        // next call on this channel, so every field is copied out here and the
        // reference is never retained past this scope.
        const auto& published = target->channel->ui_read_latest_snapshot();
        const auto schema_version = published.schema_version;
        const auto epoch = published.epoch;
        const auto engine_sequence = published.engine_sequence;
        const auto active_pattern = published.active_pattern;
        const auto active_lane_count = published.active_lane_count;
        const auto active_pattern_count = published.active_pattern_count;
        const std::uint8_t pattern_index = pattern_selected ? selected_pattern : active_pattern;
        if (pattern_index >= kPatterns)
            return fail(ControlResultCode::InvalidRequest,
                        "sequencer pattern index exceeded the channel capacity");
        state::PatternState pattern_copy{};
        if (include_snapshot) pattern_copy = published.patterns[pattern_index];

        const auto resync_epoch = target->channel->ui_resync_required_epoch();
        state::PlayheadState playhead{};
        if (include_playhead) playhead = target->channel->ui_read_playhead();

        checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_outcome(checkpoint, "sequencer state read");

        auto detail = choc::value::createObject("ControlSequencerStateReadResult");
        detail.setMember("schema_version", static_cast<std::int64_t>(schema_version));
        detail.setMember("epoch", static_cast<std::int64_t>(epoch));
        detail.setMember("engine_sequence", static_cast<std::int64_t>(engine_sequence));
        detail.setMember("active_pattern", static_cast<std::int64_t>(active_pattern));
        detail.setMember("active_lane_count", static_cast<std::int64_t>(active_lane_count));
        detail.setMember("active_pattern_count", static_cast<std::int64_t>(active_pattern_count));
        detail.setMember("resync_required_epoch", static_cast<std::int64_t>(resync_epoch));
        detail.setMember("snapshot_included", include_snapshot);
        detail.setMember("playhead_included", include_playhead);
        if (include_snapshot) {
            auto lanes = choc::value::createEmptyArray();
            const std::uint8_t lane_bound =
                active_lane_count < kLanes ? active_lane_count : kLanes;
            for (std::uint8_t lane = 0; lane < lane_bound; ++lane) {
                auto steps = choc::value::createEmptyArray();
                for (std::uint8_t step = 0; step < kSteps; ++step)
                    steps.addArrayElement(cell_to_value(pattern_copy.lanes[lane][step]));
                lanes.addArrayElement(std::move(steps));
            }
            auto pattern_value = choc::value::createObject("SequencerPatternState");
            pattern_value.setMember("index", static_cast<std::int64_t>(pattern_index));
            pattern_value.setMember("length", static_cast<std::int64_t>(pattern_copy.length));
            pattern_value.setMember("lanes", std::move(lanes));
            detail.setMember("pattern", std::move(pattern_value));
        }
        if (include_playhead) {
            auto playhead_value = choc::value::createObject("SequencerPlayheadState");
            playhead_value.setMember("sample_time",
                                     static_cast<std::int64_t>(playhead.sample_time));
            playhead_value.setMember("ppq_position", playhead.ppq_position);
            playhead_value.setMember("block_index", static_cast<std::int64_t>(playhead.block_index));
            playhead_value.setMember("active_pattern",
                                     static_cast<std::int64_t>(playhead.active_pattern));
            playhead_value.setMember("active_step", static_cast<std::int64_t>(playhead.active_step));
            playhead_value.setMember("playing", playhead.playing != 0);
            detail.setMember("playhead", std::move(playhead_value));
        }
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    };
}

ControlOperationExecutor
make_control_sequencer_state_edit_executor(ControlSequencerStateTargetResolver resolve_target) {
    auto next_client_sequence = std::make_shared<std::atomic<std::uint64_t>>(0);
    return [resolve_target = std::move(resolve_target),
            next_client_sequence = std::move(next_client_sequence)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.sequencer/state.edit@1" ||
            request.operation_version != 1 ||
            request.registration_id != plan.registration_id.value || !resolve_target ||
            !context.checkpoint) {
            return fail(ControlResultCode::InvalidRequest,
                        "sequencer state edit executor is unavailable for this operation");
        }

        state::StepEditCommand command{};
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject())
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer state edit request was not canonical");
            bool ok = true;
            bool present = false;
            const auto kind = read_string(params, "kind", ok);
            if (!ok || kind.empty())
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer edit kind is required");
            const auto phase_id = read_string(params, "gesture_phase", ok);
            if (!ok)
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer gesture phase must be a string");
            if (!phase_id.empty() && !gesture_phase_from_id(phase_id, command.gesture_phase))
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer gesture phase is not a declared enumerator");
            const auto transaction =
                read_uint(params, "transaction_id", 0xffffffffull, ok, present);
            if (!ok)
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer transaction id exceeded schema bounds");
            command.transaction_id = static_cast<state::EditTransactionId>(transaction);

            const auto pattern =
                read_uint(params, "pattern", static_cast<std::uint64_t>(kPatterns - 1), ok, present);
            const bool pattern_present = present;
            const auto lane =
                read_uint(params, "lane", static_cast<std::uint64_t>(kLanes - 1), ok, present);
            const bool lane_present = present;
            const auto step =
                read_uint(params, "step", static_cast<std::uint64_t>(kSteps - 1), ok, present);
            const bool step_present = present;
            if (!ok)
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer edit indices exceeded the channel dimensions");

            if (kind == "set-cell") {
                if (!pattern_present || !lane_present || !step_present ||
                    !params.hasObjectMember("cell"))
                    return fail(ControlResultCode::InvalidRequest,
                                "set-cell requires pattern, lane, step and cell");
                state::StepCell cell{};
                if (!value_to_cell(params["cell"], cell))
                    return fail(ControlResultCode::InvalidRequest,
                                "set-cell payload was not a canonical step cell");
                command.kind = state::StepEditKind::SetCell;
                command.payload.set_cell = state::SetCellEdit{
                    static_cast<std::uint8_t>(pattern), static_cast<std::uint8_t>(lane),
                    static_cast<std::uint8_t>(step), cell};
            } else if (kind == "clear") {
                const auto scope_id = read_string(params, "scope", ok);
                state::ClearScope scope = state::ClearScope::Cell;
                if (!ok || scope_id.empty() || !clear_scope_from_id(scope_id, scope))
                    return fail(ControlResultCode::InvalidRequest,
                                "clear requires a declared scope enumerator");
                // A narrower scope addresses more of the grid, so every index it
                // reads must be supplied. Defaulting an absent index to zero would
                // silently clear a cell the caller never named, and nothing
                // downstream re-validates a command once it is on the channel.
                const bool scope_needs_pattern = scope != state::ClearScope::All;
                const bool scope_needs_lane = scope == state::ClearScope::Lane ||
                                              scope == state::ClearScope::Cell;
                const bool scope_needs_step = scope == state::ClearScope::Cell;
                if ((scope_needs_pattern && !pattern_present) ||
                    (scope_needs_lane && !lane_present) || (scope_needs_step && !step_present))
                    return fail(ControlResultCode::InvalidRequest,
                                "clear scope requires every index it addresses");
                command.kind = state::StepEditKind::Clear;
                command.payload.clear = state::ClearEdit{
                    scope, static_cast<std::uint8_t>(pattern), static_cast<std::uint8_t>(lane),
                    static_cast<std::uint8_t>(step)};
            } else if (kind == "randomize-lane") {
                if (!pattern_present || !lane_present)
                    return fail(ControlResultCode::InvalidRequest,
                                "randomize-lane requires pattern and lane");
                bool seed_present = false;
                const auto seed = read_uint(params, "seed", 0xffffffffull, ok, seed_present);
                state::RandomizeLaneEdit edit{};
                edit.pattern = static_cast<std::uint8_t>(pattern);
                edit.lane = static_cast<std::uint8_t>(lane);
                edit.seed = static_cast<std::uint32_t>(seed);
                const auto density = read_uint(params, "density", 255, ok, present);
                if (present) edit.density = static_cast<std::uint8_t>(density);
                const auto min_velocity = read_uint(params, "min_velocity", 255, ok, present);
                if (present) edit.min_velocity = static_cast<std::uint8_t>(min_velocity);
                const auto max_velocity = read_uint(params, "max_velocity", 255, ok, present);
                if (present) edit.max_velocity = static_cast<std::uint8_t>(max_velocity);
                if (!ok || !seed_present)
                    return fail(ControlResultCode::InvalidRequest,
                                "randomize-lane payload exceeded schema bounds");
                if (edit.min_velocity > edit.max_velocity)
                    return fail(ControlResultCode::InvalidRequest,
                                "randomize-lane velocity range is inverted");
                command.kind = state::StepEditKind::RandomizeLane;
                command.payload.randomize_lane = edit;
            } else if (kind == "set-pattern-length") {
                bool length_present = false;
                const auto length = read_uint(params, "length", static_cast<std::uint64_t>(kSteps),
                                              ok, length_present);
                if (!ok || !pattern_present || !length_present)
                    return fail(ControlResultCode::InvalidRequest,
                                "set-pattern-length requires pattern and a bounded length");
                command.kind = state::StepEditKind::SetPatternLength;
                command.payload.set_pattern_length = state::SetPatternLengthEdit{
                    static_cast<std::uint8_t>(pattern), static_cast<std::uint8_t>(length)};
            } else if (kind == "switch-pattern") {
                if (!pattern_present)
                    return fail(ControlResultCode::InvalidRequest,
                                "switch-pattern requires a pattern");
                command.kind = state::StepEditKind::SwitchPattern;
                command.payload.switch_pattern =
                    state::SwitchPatternEdit{static_cast<std::uint8_t>(pattern)};
            } else {
                return fail(ControlResultCode::InvalidRequest,
                            "sequencer edit kind is not a declared enumerator");
            }
        } catch (...) {
            return fail(ControlResultCode::InvalidRequest,
                        "sequencer state edit request could not be decoded");
        }

        auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_outcome(checkpoint, "sequencer state edit");
        if (off_host_main_thread())
            return fail(ControlResultCode::HostUnavailable,
                        "sequencer state edit requires the host main thread",
                        ControlRetryClassification::AfterBackoff);

        auto target = resolve(resolve_target, plan);
        if (!target)
            return fail(ControlResultCode::HostUnavailable,
                        "exact sequencer channel is unavailable",
                        ControlRetryClassification::AfterRefresh);

        checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_outcome(checkpoint, "sequencer state edit");

        const auto client_sequence = next_client_sequence->fetch_add(1) + 1;
        if (client_sequence >=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return fail(ControlResultCode::ResourceExhausted,
                        "sequencer client sequence exceeds the canonical wire range");
        command.client_sequence = static_cast<state::ClientSequence>(client_sequence);

        // A full command FIFO is a bounded, typed, retryable refusal. Blocking
        // or dropping here would either stall the host main thread or silently
        // lose an edit the caller was told had been accepted.
        if (!target->channel->ui_try_submit(command))
            return fail(ControlResultCode::ResourceExhausted,
                        "sequencer command queue is full", ControlRetryClassification::AfterBackoff);

        auto detail = choc::value::createObject("ControlSequencerStateEditResult");
        detail.setMember("receipt_id", plan.receipt_id.value);
        detail.setMember("applied", true);
        detail.setMember("client_sequence", static_cast<std::int64_t>(client_sequence));
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    };
}

} // namespace pulp::inspect
