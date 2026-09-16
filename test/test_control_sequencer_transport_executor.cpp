#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_sequencer_transport_executor.hpp>
#include <pulp/playback/transport.hpp>

#include "timebase_test_helpers.hpp"

#include <choc/text/choc_JSON.h>

#include <array>
#include <cstdint>
#include <string>

using namespace pulp;
using namespace pulp::inspect;

namespace {

timebase::CompiledTempoMap constant_map() {
    const std::array points{timebase::TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, timebase::RationalRate{48'000, 1});
}

ControlAdmissionPlan plan() {
    ControlAdmissionPlan value;
    value.registration_id = ControlRegistrationId{"registration-1"};
    value.receipt_id = ControlReceiptId{"receipt-1"};
    return value;
}

ControlRequestEnvelope read_request() {
    return {.registration_id = "registration-1",
            .operation_id = "dev.pulp.sequencer/transport.loop.read@1",
            .operation_version = 1,
            .params_json = "{}"};
}

ControlRequestEnvelope write_request(std::string params) {
    return {.registration_id = "registration-1",
            .operation_id = "dev.pulp.sequencer/transport.loop.write@1",
            .operation_version = 1,
            .params_json = std::move(params)};
}

ControlExecutionContext context() {
    return {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }};
}

ControlSequencerTransportTargetResolver bound(playback::MasterTransport& transport) {
    return [&transport](const ControlAdmissionPlan& admitted)
               -> std::optional<ControlSequencerTransportTarget> {
        return ControlSequencerTransportTarget{.registration_id = admitted.registration_id,
                                               .host_tier = ControlHostTier::Standalone,
                                               .transport = &transport};
    };
}

choc::value::Value detail_of(const ControlExecutionOutcome& outcome) {
    return choc::json::parse(outcome.result.detail_json);
}

/// A prepared transport whose tick range spans whole quarter notes, so every
/// tick used below maps to an exact sample and the transport's own sample-domain
/// rejections are the only ones that can fire.
struct PreparedTransport {
    timebase::CompiledTempoMap map = constant_map();
    playback::MasterTransport transport;

    PreparedTransport() {
        playback::MasterTransportConfig setup;
        setup.max_buffer_size = 512;
        REQUIRE(transport.prepare(map, setup) == playback::TransportError::None);
    }

    std::int64_t tick_at_sample(std::int64_t sample) {
        const auto tick = map.samples_to_ticks({sample});
        REQUIRE(map.ticks_to_samples(tick) == timebase::SamplePosition{sample});
        return tick.value;
    }

    void run_block(std::uint32_t frames) {
        playback::TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(frames, snapshot) == playback::TransportError::None);
    }
};

} // namespace

TEST_CASE("transport loop read reports the transport's own publication",
          "[inspect][control][sequencer][transport][read]") {
    PreparedTransport fixture;
    const auto start = fixture.tick_at_sample(0);
    const auto end = fixture.tick_at_sample(96'000);
    REQUIRE(fixture.transport.set_loop(
                {true, timebase::TickPosition{start}, timebase::TickPosition{end}}) ==
            playback::TransportError::None);
    fixture.run_block(256);

    auto executor = make_control_sequencer_transport_read_executor(bound(fixture.transport));
    const auto outcome = executor(plan(), read_request(), context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto detail = detail_of(outcome);
    CHECK(detail["start_tick"].getInt64() == start);
    CHECK(detail["end_tick"].getInt64() == end);
    CHECK(detail["enabled"].getBool());
    CHECK(detail["sequence"].getInt64() >= 1);
    CHECK_FALSE(detail.hasObjectMember("receipt_id"));
}

TEST_CASE("transport loop read refuses an unprepared transport rather than inventing a loop",
          "[inspect][control][sequencer][transport][read]") {
    playback::MasterTransport transport;
    auto executor = make_control_sequencer_transport_read_executor(bound(transport));
    const auto outcome = executor(plan(), read_request(), context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(outcome.result.explanation.find("published no state") != std::string::npos);
}

TEST_CASE("transport loop read refuses when no transport is bound",
          "[inspect][control][sequencer][transport][read]") {
    auto executor = make_control_sequencer_transport_read_executor(
        [](const ControlAdmissionPlan&) -> std::optional<ControlSequencerTransportTarget> {
            return std::nullopt;
        });
    const auto outcome = executor(plan(), read_request(), context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::HostUnavailable);
}

TEST_CASE("set-range receipt carries the transport's acceptance, not the tick helper's",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    const auto start = fixture.tick_at_sample(0);
    const auto end = fixture.tick_at_sample(96'000);
    const auto outcome = executor(
        plan(),
        write_request(R"({"action":"set-range","start_tick":)" + std::to_string(start) +
                      R"(,"end_tick":)" + std::to_string(end) + R"(,"idempotency_key":"once"})"),
        context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto detail = detail_of(outcome);
    CHECK(detail["receipt_id"].getString() == "receipt-1");
    CHECK(detail["action"].getString() == "set-range");
    CHECK(detail["applied"].getBool());
    CHECK(detail["start_tick"].getInt64() == start);
    CHECK(detail["end_tick"].getInt64() == end);

    // The transport, not the executor, is the authority on what was accepted.
    fixture.run_block(256);
    const auto published = fixture.transport.playhead();
    CHECK(published.loop.start.value == start);
    CHECK(published.loop.end.value == end);
    CHECK(published.loop.enabled);
}

TEST_CASE("a range the tick helper accepts is still refused with the transport's own verdict",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    // Two distinct ticks (the tick helper is satisfied) whose sample span is
    // shorter than the prepared 512-frame maximum block (the transport is not).
    const auto start = fixture.tick_at_sample(0);
    const auto end = fixture.tick_at_sample(240);
    REQUIRE(end != start);
    const auto outcome = executor(
        plan(),
        write_request(R"({"action":"set-range","start_tick":)" + std::to_string(start) +
                      R"(,"end_tick":)" + std::to_string(end) + R"(,"idempotency_key":"once"})"),
        context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(outcome.result.explanation.find("the transport rejected the loop") != std::string::npos);
    CHECK(outcome.result.explanation.find("shorter than the prepared maximum block") !=
          std::string::npos);
    CHECK(outcome.result.explanation.find("same document tick") == std::string::npos);
}

TEST_CASE("collapsed endpoints get the tick helper's own distinct refusal",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    const auto outcome = executor(
        plan(),
        write_request(
            R"({"action":"set-range","start_tick":480,"end_tick":480,"idempotency_key":"once"})"),
        context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(outcome.result.explanation.find("same document tick") != std::string::npos);
    CHECK(outcome.result.explanation.find("the transport rejected") == std::string::npos);
}

TEST_CASE("set-enabled refuses endpoints instead of snapping or re-ranging",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    const auto outcome =
        executor(plan(),
                 write_request(R"({"action":"set-enabled","enabled":false,"expected_sequence":1,)"
                               R"("start_tick":0,"end_tick":480,"idempotency_key":"once"})"),
                 context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(outcome.result.explanation.find("carries no endpoints") != std::string::npos);
}

TEST_CASE("set-enabled preserves an accepted range the audio thread has not yet published",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    const auto start = fixture.tick_at_sample(0);
    const auto end = fixture.tick_at_sample(96'000);
    const auto ranged = executor(
        plan(),
        write_request(R"({"action":"set-range","start_tick":)" + std::to_string(start) +
                      R"(,"end_tick":)" + std::to_string(end) + R"(,"idempotency_key":"once"})"),
        context());
    REQUIRE(ranged.terminal_state == ControlReceiptState::Completed);
    const auto sequence = detail_of(ranged)["sequence"].getInt64();

    // No block has run, so the published playhead still carries the previous
    // loop. Toggling must not discard the range the transport already accepted.
    const auto toggled =
        executor(plan(),
                 write_request(R"({"action":"set-enabled","enabled":false,"expected_sequence":)" +
                               std::to_string(sequence) + R"(,"idempotency_key":"twice"})"),
                 context());
    REQUIRE(toggled.terminal_state == ControlReceiptState::Completed);
    const auto detail = detail_of(toggled);
    CHECK(detail["action"].getString() == "set-enabled");
    CHECK_FALSE(detail["enabled"].getBool());
    CHECK(detail["start_tick"].getInt64() == start);
    CHECK(detail["end_tick"].getInt64() == end);

    fixture.run_block(256);
    const auto published = fixture.transport.playhead();
    CHECK(published.loop.start.value == start);
    CHECK(published.loop.end.value == end);
    CHECK_FALSE(published.loop.enabled);
}

TEST_CASE("set-enabled refuses a stale publication sequence",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    fixture.run_block(256);
    const auto stale = fixture.transport.playhead().sequence;
    fixture.run_block(256);
    REQUIRE(fixture.transport.playhead().sequence != stale);

    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    const auto outcome =
        executor(plan(),
                 write_request(R"({"action":"set-enabled","enabled":true,"expected_sequence":)" +
                               std::to_string(static_cast<std::int64_t>(stale)) +
                               R"(,"idempotency_key":"once"})"),
                 context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::StateConflict);
}

TEST_CASE("an unknown write action is refused rather than defaulted",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    const auto outcome = executor(
        plan(), write_request(R"({"action":"set-tempo","idempotency_key":"once"})"), context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(outcome.result.explanation.find("not a known branch") != std::string::npos);
}

TEST_CASE("a mismatched registration is refused before the transport is touched",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(
        [&fixture](const ControlAdmissionPlan&) -> std::optional<ControlSequencerTransportTarget> {
            return ControlSequencerTransportTarget{.registration_id =
                                                       ControlRegistrationId{"registration-other"},
                                                   .host_tier = ControlHostTier::Standalone,
                                                   .transport = &fixture.transport};
        });
    const auto start = fixture.tick_at_sample(0);
    const auto end = fixture.tick_at_sample(96'000);
    const auto outcome = executor(
        plan(),
        write_request(R"({"action":"set-range","start_tick":)" + std::to_string(start) +
                      R"(,"end_tick":)" + std::to_string(end) + R"(,"idempotency_key":"once"})"),
        context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::HostUnavailable);
    CHECK(fixture.transport.playhead().loop.enabled == false);
}

TEST_CASE("a set-range endpoint beyond the wire range is refused before the transport is touched",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    const auto outcome = executor(
        plan(),
        write_request(R"({"action":"set-range","start_tick":0,"end_tick":9007199254740992,)"
                      R"("idempotency_key":"once"})"),
        context());
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(outcome.result.explanation.find("exceeded the canonical wire range") !=
          std::string::npos);
    CHECK_FALSE(fixture.transport.playhead().loop.enabled);
}

TEST_CASE("a negative document tick is carried rather than refused as out of range",
          "[inspect][control][sequencer][transport][write]") {
    PreparedTransport fixture;
    auto executor = make_control_sequencer_transport_write_executor(bound(fixture.transport));
    // The canonical tick for sample zero is itself negative, so a loop anchored
    // at the start of the timeline must survive the wire.
    const auto start = fixture.tick_at_sample(0);
    REQUIRE(start < 0);
    const auto end = fixture.tick_at_sample(96'000);
    const auto outcome = executor(
        plan(),
        write_request(R"({"action":"set-range","start_tick":)" + std::to_string(start) +
                      R"(,"end_tick":)" + std::to_string(end) + R"(,"idempotency_key":"once"})"),
        context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    CHECK(detail_of(outcome)["start_tick"].getInt64() == start);
    fixture.run_block(256);
    CHECK(fixture.transport.playhead().loop.start.value == start);
}
