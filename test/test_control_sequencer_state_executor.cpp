#include <catch2/catch_test_macros.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/control_sequencer_state_executor.hpp>

#include <choc/text/choc_JSON.h>

#include <optional>
#include <string>

using namespace pulp::inspect;
namespace state = pulp::state;
namespace events = pulp::events;

namespace {

ControlAdmissionPlan plan() {
    ControlAdmissionPlan value;
    value.registration_id = ControlRegistrationId{"registration-1"};
    value.receipt_id = ControlReceiptId{"receipt-1"};
    return value;
}

ControlRequestEnvelope read_request(std::string params = "{}") {
    return {.registration_id = "registration-1",
            .operation_id = "dev.pulp.sequencer/state.read@1",
            .operation_version = 1,
            .params_json = std::move(params)};
}

ControlRequestEnvelope edit_request(std::string params) {
    return {.registration_id = "registration-1",
            .operation_id = "dev.pulp.sequencer/state.edit@1",
            .operation_version = 1,
            .params_json = std::move(params)};
}

ControlExecutionContext context() {
    return {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }};
}

ControlSequencerStateTargetResolver resolver(state::SequencerStateChannel& channel,
                                             ControlHostTier tier = ControlHostTier::Standalone) {
    return [&channel, tier](const ControlAdmissionPlan& admitted)
               -> std::optional<ControlSequencerStateTarget> {
        return ControlSequencerStateTarget{.registration_id = admitted.registration_id,
                                           .host_tier = tier,
                                           .channel = &channel};
    };
}

/// Publishes a distinguishable engine snapshot so a read that never touched the
/// triple buffer cannot accidentally match the default-constructed state.
void publish_seeded_snapshot(state::SequencerStateChannel& channel) {
    state::Snapshot snapshot{};
    snapshot.epoch = 9;
    snapshot.engine_sequence = 41;
    snapshot.active_pattern = 2;
    snapshot.active_lane_count = 3;
    snapshot.active_pattern_count = 5;
    snapshot.patterns[2].length = 16;
    snapshot.patterns[2].lanes[1][4] = state::StepCell{.flags = state::StepCell::kEnabledBit,
                                                       .velocity = 111,
                                                       .probability = 96,
                                                       .pitch_offset = -5,
                                                       .gate_ticks = 33,
                                                       .ratchet = 3};
    channel.audio_publish_snapshot(snapshot);
}

/// Installs a dispatcher backend that always reports "not the main thread", so
/// the runtime half of the HostMain binding is exercised without a real loop.
class OffMainThreadBackend {
public:
    OffMainThreadBackend()
        : token_(events::MainThreadDispatcher::register_backend(
              {.post = [](events::Task) { return false; },
               .is_main_thread = [] { return false; },
               .post_after = {}})) {}
    ~OffMainThreadBackend() { events::MainThreadDispatcher::unregister_backend(token_); }
    OffMainThreadBackend(const OffMainThreadBackend&) = delete;
    OffMainThreadBackend& operator=(const OffMainThreadBackend&) = delete;

private:
    events::MainThreadDispatcher::Token token_;
};

const InspectorCapabilityDescriptor& descriptor(InspectorCapability capability) {
    for (const auto& entry : inspector_capability_registry())
        if (entry.capability == capability)
            return entry;
    throw std::runtime_error("capability is not registered");
}

} // namespace

TEST_CASE("sequencer state capabilities declare the HostMain executor binding",
          "[inspect][control][sequencer][main-thread][registry]") {
    const auto& read = descriptor(InspectorCapability::SequencerStateRead);
    const auto& edit = descriptor(InspectorCapability::SequencerStateEdit);

    // The declared binding is the half a later refactor can silently flip; the
    // off-thread refusal below is the runtime half of the same contract.
    CHECK(read.executor == InspectorExecutor::HostMain);
    CHECK(edit.executor == InspectorExecutor::HostMain);
    CHECK(executor_id(read.executor) == "host-main");
    CHECK(executor_id(edit.executor) == "host-main");

    CHECK(read.contract_id == "dev.pulp.sequencer/state.read@1");
    CHECK(edit.contract_id == "dev.pulp.sequencer/state.edit@1");
    CHECK(read.risk == InspectorCapabilityRisk::Sensitive);
    CHECK(edit.risk == InspectorCapabilityRisk::Control);
    CHECK(read.side_effect == InspectorSideEffect::None);
    CHECK(edit.side_effect == InspectorSideEffect::State);
    CHECK(read.evidence == InspectorEvidence::Response);
    CHECK(edit.evidence == InspectorEvidence::Receipt);

    // Grant-profile membership IS the observe/develop pair; a read that leaves
    // the observe profile would silently narrow the surface Forge consumes.
    CHECK(read.in_observe_profile);
    CHECK(read.in_develop_profile);
    CHECK_FALSE(edit.in_observe_profile);
    CHECK(edit.in_develop_profile);
    CHECK(read.grantable);
    CHECK(edit.grantable);
}

TEST_CASE("sequencer state read copies the published snapshot and playhead",
          "[inspect][control][sequencer][read]") {
    state::SequencerStateChannel channel;
    publish_seeded_snapshot(channel);
    state::PlayheadState playhead{};
    playhead.sample_time = 4096;
    playhead.ppq_position = 2.5;
    playhead.block_index = 7;
    playhead.active_pattern = 2;
    playhead.active_step = 11;
    playhead.playing = 1;
    channel.audio_publish_playhead(playhead);

    auto executor = make_control_sequencer_state_read_executor(resolver(channel));
    const auto outcome = executor(plan(), read_request(), context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);

    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["epoch"].getWithDefault<std::int64_t>(0) == 9);
    CHECK(detail["engine_sequence"].getWithDefault<std::int64_t>(0) == 41);
    CHECK(detail["active_pattern"].getWithDefault<std::int64_t>(0) == 2);
    CHECK(detail["active_lane_count"].getWithDefault<std::int64_t>(0) == 3);
    CHECK(detail["active_pattern_count"].getWithDefault<std::int64_t>(0) == 5);
    CHECK(detail["snapshot_included"].getWithDefault(false));
    CHECK(detail["playhead_included"].getWithDefault(false));

    const auto pattern = detail["pattern"];
    CHECK(pattern["index"].getWithDefault<std::int64_t>(-1) == 2);
    CHECK(pattern["length"].getWithDefault<std::int64_t>(0) == 16);
    // active_lane_count bounds the projection, so lane 2 is the last emitted.
    REQUIRE(pattern["lanes"].size() == 3);
    REQUIRE(pattern["lanes"][1].size() == 32);
    const auto cell = pattern["lanes"][1][4];
    CHECK(cell["enabled"].getWithDefault(false));
    CHECK(cell["velocity"].getWithDefault<std::int64_t>(0) == 111);
    CHECK(cell["probability"].getWithDefault<std::int64_t>(0) == 96);
    CHECK(cell["pitch_offset"].getWithDefault<std::int64_t>(0) == -5);
    CHECK(cell["gate_ticks"].getWithDefault<std::int64_t>(0) == 33);
    CHECK(cell["ratchet"].getWithDefault<std::int64_t>(0) == 3);
    CHECK_FALSE(pattern["lanes"][0][0]["enabled"].getWithDefault(true));

    const auto reported = detail["playhead"];
    CHECK(reported["sample_time"].getWithDefault<std::int64_t>(0) == 4096);
    CHECK(reported["block_index"].getWithDefault<std::int64_t>(0) == 7);
    CHECK(reported["active_step"].getWithDefault<std::int64_t>(0) == 11);
    CHECK(reported["playing"].getWithDefault(false));

    // A read must never enqueue a command and must never consume the applied
    // echo the host's own single UI consumer owns.
    CHECK_FALSE(channel.audio_try_pop_command().has_value());
}

TEST_CASE("sequencer state read honours explicit projection selectors",
          "[inspect][control][sequencer][read]") {
    state::SequencerStateChannel channel;
    publish_seeded_snapshot(channel);
    auto executor = make_control_sequencer_state_read_executor(resolver(channel));

    const auto outcome = executor(
        plan(), read_request(R"({"include_snapshot":false,"include_playhead":false})"), context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK_FALSE(detail["snapshot_included"].getWithDefault(true));
    CHECK_FALSE(detail["playhead_included"].getWithDefault(true));
    CHECK_FALSE(detail.hasObjectMember("pattern"));
    CHECK_FALSE(detail.hasObjectMember("playhead"));

    const auto selected =
        executor(plan(), read_request(R"({"pattern":4,"include_playhead":false})"), context());
    REQUIRE(selected.terminal_state == ControlReceiptState::Completed);
    const auto selected_detail = choc::json::parse(selected.result.detail_json);
    CHECK(selected_detail["pattern"]["index"].getWithDefault<std::int64_t>(-1) == 4);

    const auto refused = executor(plan(), read_request(R"({"pattern":99})"), context());
    CHECK(refused.terminal_state == ControlReceiptState::Failed);
    CHECK(refused.result.result_code == ControlResultCode::InvalidRequest);
}

TEST_CASE("sequencer state edit submits one exact typed command",
          "[inspect][control][sequencer][edit][mutation]") {
    state::SequencerStateChannel channel;
    auto executor = make_control_sequencer_state_edit_executor(resolver(channel));

    const auto outcome = executor(
        plan(),
        edit_request(
            R"({"kind":"set-cell","gesture_phase":"update","transaction_id":77,"pattern":3,)"
            R"("lane":2,"step":5,"cell":{"enabled":true,"velocity":120,"probability":64,)"
            R"("pitch_offset":-3,"gate_ticks":48,"ratchet":2}})"),
        context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["receipt_id"].getString() == "receipt-1");
    CHECK(detail["applied"].getWithDefault(false));
    CHECK(detail["client_sequence"].getWithDefault<std::int64_t>(0) == 1);

    const auto command = channel.audio_try_pop_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == state::StepEditKind::SetCell);
    CHECK(command->gesture_phase == state::GesturePhase::Update);
    CHECK(command->transaction_id == 77);
    CHECK(command->client_sequence == 1);
    CHECK(command->payload.set_cell.pattern == 3);
    CHECK(command->payload.set_cell.lane == 2);
    CHECK(command->payload.set_cell.step == 5);
    CHECK(command->payload.set_cell.cell.enabled());
    CHECK(command->payload.set_cell.cell.velocity == 120);
    CHECK(command->payload.set_cell.cell.probability == 64);
    CHECK(command->payload.set_cell.cell.pitch_offset == -3);
    CHECK(command->payload.set_cell.cell.gate_ticks == 48);
    CHECK(command->payload.set_cell.cell.ratchet == 2);

    // The client sequence is monotonic per executor, so the engine can order
    // two edits that arrive in the same host-main turn.
    const auto second = executor(
        plan(), edit_request(R"({"kind":"switch-pattern","pattern":7})"), context());
    REQUIRE(second.terminal_state == ControlReceiptState::Completed);
    const auto second_command = channel.audio_try_pop_command();
    REQUIRE(second_command.has_value());
    CHECK(second_command->kind == state::StepEditKind::SwitchPattern);
    CHECK(second_command->payload.switch_pattern.pattern == 7);
    CHECK(second_command->client_sequence == 2);
}

TEST_CASE("sequencer state edit encodes every declared edit kind",
          "[inspect][control][sequencer][edit][mutation]") {
    state::SequencerStateChannel channel;
    auto executor = make_control_sequencer_state_edit_executor(resolver(channel));

    REQUIRE(executor(plan(),
                     edit_request(R"({"kind":"clear","scope":"lane","pattern":1,"lane":4})"),
                     context())
                .terminal_state == ControlReceiptState::Completed);
    auto command = channel.audio_try_pop_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == state::StepEditKind::Clear);
    CHECK(command->payload.clear.scope == state::ClearScope::Lane);
    CHECK(command->payload.clear.pattern == 1);
    CHECK(command->payload.clear.lane == 4);

    REQUIRE(executor(plan(),
                     edit_request(R"({"kind":"randomize-lane","pattern":2,"lane":6,"seed":1234,)"
                                  R"("density":90,"min_velocity":40,"max_velocity":110})"),
                     context())
                .terminal_state == ControlReceiptState::Completed);
    command = channel.audio_try_pop_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == state::StepEditKind::RandomizeLane);
    CHECK(command->payload.randomize_lane.pattern == 2);
    CHECK(command->payload.randomize_lane.lane == 6);
    CHECK(command->payload.randomize_lane.seed == 1234);
    CHECK(command->payload.randomize_lane.density == 90);
    CHECK(command->payload.randomize_lane.min_velocity == 40);
    CHECK(command->payload.randomize_lane.max_velocity == 110);

    REQUIRE(executor(plan(),
                     edit_request(R"({"kind":"set-pattern-length","pattern":5,"length":12})"),
                     context())
                .terminal_state == ControlReceiptState::Completed);
    command = channel.audio_try_pop_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == state::StepEditKind::SetPatternLength);
    CHECK(command->payload.set_pattern_length.pattern == 5);
    CHECK(command->payload.set_pattern_length.length == 12);

    // The widest scope addresses no index, so it stays accepted without one.
    // This is what keeps the index requirement scope-exact rather than blanket.
    REQUIRE(executor(plan(), edit_request(R"({"kind":"clear","scope":"all"})"), context())
                .terminal_state == ControlReceiptState::Completed);
    command = channel.audio_try_pop_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == state::StepEditKind::Clear);
    CHECK(command->payload.clear.scope == state::ClearScope::All);
}

TEST_CASE("sequencer state edit refuses a malformed or out-of-range request",
          "[inspect][control][sequencer][edit][refusal]") {
    state::SequencerStateChannel channel;
    auto executor = make_control_sequencer_state_edit_executor(resolver(channel));

    const auto refuse = [&](const char* params) {
        const auto outcome = executor(plan(), edit_request(params), context());
        CHECK(outcome.terminal_state == ControlReceiptState::Failed);
        CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    };

    refuse(R"({"kind":"teleport-lane","pattern":0,"lane":0})");
    refuse(R"({"kind":"set-cell","pattern":0,"lane":0})");
    refuse(R"({"kind":"set-cell","pattern":0,"lane":0,"step":0,"cell":{"enabled":true}})");
    refuse(R"({"kind":"switch-pattern","pattern":32})");
    refuse(R"({"kind":"switch-pattern","pattern":0,"gesture_phase":"levitate"})");
    refuse(R"({"kind":"clear","pattern":0})");
    // A clear scope must carry every index it addresses; an absent index must
    // never be defaulted into clearing a cell the caller did not name.
    refuse(R"({"kind":"clear","scope":"cell"})");
    refuse(R"({"kind":"clear","scope":"cell","pattern":0,"lane":0})");
    refuse(R"({"kind":"clear","scope":"lane","pattern":0})");
    refuse(R"({"kind":"clear","scope":"pattern"})");
    refuse(R"({"kind":"randomize-lane","pattern":0,"lane":0})");
    refuse(R"({"kind":"randomize-lane","pattern":0,"lane":0,"seed":1,)"
           R"("min_velocity":120,"max_velocity":30})");
    refuse(R"({"kind":"set-pattern-length","pattern":0})");
    refuse(R"({"kind":"set-cell","pattern":0,"lane":12,"step":0,)"
           R"("cell":{"enabled":true,"velocity":1,"probability":1,"pitch_offset":0,)"
           R"("gate_ticks":1,"ratchet":1}})");
    refuse("not json at all");

    // Every refusal above must have been decided before anything reached the
    // lock-free FIFO.
    CHECK_FALSE(channel.audio_try_pop_command().has_value());
}

TEST_CASE("a full sequencer command FIFO is a typed retryable refusal and never a drop",
          "[inspect][control][sequencer][edit][backpressure]") {
    state::SequencerStateChannel channel;
    auto executor = make_control_sequencer_state_edit_executor(resolver(channel));
    const auto request = edit_request(R"({"kind":"switch-pattern","pattern":1})");

    std::size_t accepted = 0;
    ControlExecutionOutcome outcome{};
    for (std::size_t attempt = 0; attempt < state::kCommandQueueCapacity + 8; ++attempt) {
        outcome = executor(plan(), request, context());
        if (outcome.terminal_state != ControlReceiptState::Completed)
            break;
        ++accepted;
    }

    CHECK(accepted > 0);
    CHECK(accepted <= state::kCommandQueueCapacity);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::ResourceExhausted);
    CHECK(outcome.result.retry == ControlRetryClassification::AfterBackoff);

    // The refusal is bounded: draining the engine side makes the very next
    // submission succeed again, which is what "retryable" has to mean.
    REQUIRE(channel.audio_try_pop_command().has_value());
    CHECK(executor(plan(), request, context()).terminal_state ==
          ControlReceiptState::Completed);
}

TEST_CASE("both sequencer operations refuse an off-host-main-thread call",
          "[inspect][control][sequencer][main-thread][refusal]") {
    state::SequencerStateChannel channel;
    publish_seeded_snapshot(channel);
    auto read = make_control_sequencer_state_read_executor(resolver(channel));
    auto edit = make_control_sequencer_state_edit_executor(resolver(channel));

    // Without a backend installed there is no host thread to be wrong about, so
    // the operations run. This is the positive control for the refusal below.
    REQUIRE_FALSE(events::MainThreadDispatcher::has_backend());
    CHECK(read(plan(), read_request(), context()).terminal_state ==
          ControlReceiptState::Completed);
    CHECK(edit(plan(), edit_request(R"({"kind":"switch-pattern","pattern":1})"), context())
              .terminal_state == ControlReceiptState::Completed);
    REQUIRE(channel.audio_try_pop_command().has_value());

    const OffMainThreadBackend backend;
    REQUIRE(events::MainThreadDispatcher::has_backend());
    REQUIRE_FALSE(events::MainThreadDispatcher::is_main_thread());

    const auto refused_read = read(plan(), read_request(), context());
    CHECK(refused_read.terminal_state == ControlReceiptState::Failed);
    CHECK(refused_read.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(refused_read.result.retry == ControlRetryClassification::AfterBackoff);

    const auto refused_edit =
        edit(plan(), edit_request(R"({"kind":"switch-pattern","pattern":2})"), context());
    CHECK(refused_edit.terminal_state == ControlReceiptState::Failed);
    CHECK(refused_edit.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(refused_edit.result.retry == ControlRetryClassification::AfterBackoff);

    // The refused edit must not have reached the single-writer command FIFO.
    CHECK_FALSE(channel.audio_try_pop_command().has_value());
}

TEST_CASE("sequencer operations refuse an unresolvable or wrongly-tiered channel",
          "[inspect][control][sequencer][refusal]") {
    state::SequencerStateChannel channel;
    publish_seeded_snapshot(channel);

    auto unbound = make_control_sequencer_state_read_executor(
        [](const ControlAdmissionPlan&) { return std::optional<ControlSequencerStateTarget>{}; });
    const auto no_target = unbound(plan(), read_request(), context());
    CHECK(no_target.terminal_state == ControlReceiptState::Failed);
    CHECK(no_target.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(no_target.result.retry == ControlRetryClassification::AfterRefresh);

    auto null_channel =
        make_control_sequencer_state_read_executor([](const ControlAdmissionPlan& admitted) {
            return std::optional{ControlSequencerStateTarget{
                .registration_id = admitted.registration_id,
                .host_tier = ControlHostTier::Standalone,
                .channel = nullptr}};
        });
    CHECK(null_channel(plan(), read_request(), context()).result.result_code ==
          ControlResultCode::HostUnavailable);

    auto foreign_registration =
        make_control_sequencer_state_read_executor([&channel](const ControlAdmissionPlan&) {
            return std::optional{ControlSequencerStateTarget{
                .registration_id = ControlRegistrationId{"registration-2"},
                .host_tier = ControlHostTier::Standalone,
                .channel = &channel}};
        });
    CHECK(foreign_registration(plan(), read_request(), context()).result.result_code ==
          ControlResultCode::HostUnavailable);

    auto untrusted =
        make_control_sequencer_state_read_executor(resolver(channel, ControlHostTier::OfflineJob));
    CHECK(untrusted(plan(), read_request(), context()).result.result_code ==
          ControlResultCode::HostUnavailable);

    auto edit = make_control_sequencer_state_edit_executor(
        resolver(channel, ControlHostTier::OfflineJob));
    CHECK(edit(plan(), edit_request(R"({"kind":"switch-pattern","pattern":1})"), context())
              .result.result_code == ControlResultCode::HostUnavailable);
    CHECK_FALSE(channel.audio_try_pop_command().has_value());
}

TEST_CASE("sequencer operations refuse a foreign operation id and honour cancellation",
          "[inspect][control][sequencer][refusal]") {
    state::SequencerStateChannel channel;
    publish_seeded_snapshot(channel);
    auto read = make_control_sequencer_state_read_executor(resolver(channel));
    auto edit = make_control_sequencer_state_edit_executor(resolver(channel));

    auto foreign = read_request();
    foreign.operation_id = "dev.pulp.sequencer/state.edit@1";
    CHECK(read(plan(), foreign, context()).result.result_code ==
          ControlResultCode::InvalidRequest);

    auto wrong_version = read_request();
    wrong_version.operation_version = 2;
    CHECK(read(plan(), wrong_version, context()).result.result_code ==
          ControlResultCode::InvalidRequest);

    auto foreign_registration = read_request();
    foreign_registration.registration_id = "registration-9";
    CHECK(read(plan(), foreign_registration, context()).result.result_code ==
          ControlResultCode::InvalidRequest);

    const ControlExecutionContext cancelled{
        .checkpoint = [] { return ControlExecutionCheckpoint::Cancelled; }};
    const auto cancelled_read = read(plan(), read_request(), cancelled);
    CHECK(cancelled_read.terminal_state == ControlReceiptState::Cancelled);
    CHECK(cancelled_read.result.cancellation_reason == "client-cancelled");

    const ControlExecutionContext revoked{
        .checkpoint = [] { return ControlExecutionCheckpoint::AuthorityRevoked; }};
    const auto revoked_edit =
        edit(plan(), edit_request(R"({"kind":"switch-pattern","pattern":1})"), revoked);
    CHECK(revoked_edit.terminal_state == ControlReceiptState::Cancelled);
    CHECK(revoked_edit.result.cancellation_reason == "authority-revoked");

    const ControlExecutionContext deadline{
        .checkpoint = [] { return ControlExecutionCheckpoint::DeadlineExceeded; }};
    CHECK(edit(plan(), edit_request(R"({"kind":"switch-pattern","pattern":1})"), deadline)
              .result.result_code == ControlResultCode::DeadlineExceeded);

    // Nothing above may have reached the command FIFO.
    CHECK_FALSE(channel.audio_try_pop_command().has_value());
}
