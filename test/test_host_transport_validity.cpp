#include <catch2/catch_test_macros.hpp>

#include <pulp/format/adapter_boundary.hpp>

using namespace pulp::format;

namespace {

ProcessContext base_context() {
    ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 64;
    ctx.process_mode = ProcessMode::Realtime;
    return ctx;
}

} // namespace

TEST_CASE("host transport mapper preserves partial valid zero values",
          "[format][boundary][transport][validity]") {
    boundary::HostTransport transport;
    transport.validity.set(TransportField::BeatPosition);
    transport.validity.set(TransportField::SamplePosition);
    transport.validity.set(TransportField::LoopRange);
    transport.validity.set(TransportField::Bar);
    transport.validity.set(TransportField::HostTime);

    ProcessContext ctx = base_context();
    detail::PlayheadSnapshot snapshot;
    boundary::apply_host_transport(ctx, transport, snapshot);

    CHECK(ctx.position_beats == 0.0);
    CHECK(ctx.position_samples == 0);
    CHECK(ctx.loop_start_beats == 0.0);
    CHECK(ctx.loop_end_beats == 0.0);
    CHECK(ctx.bar == 0);
    CHECK(ctx.host_time_ns == 0);
    CHECK(ctx.has_transport(TransportField::BeatPosition));
    CHECK(ctx.has_transport(TransportField::SamplePosition));
    CHECK(ctx.has_transport(TransportField::LoopRange));
    CHECK(ctx.has_transport(TransportField::Bar));
    CHECK(ctx.has_transport(TransportField::HostTime));
    CHECK_FALSE(ctx.has_transport(TransportField::Playing));
    CHECK_FALSE(ctx.has_transport(TransportField::Tempo));
    CHECK_FALSE(ctx.has_transport(TransportField::TimeSignature));
    CHECK_FALSE(ctx.has_transport(TransportField::FrameRate));
}

TEST_CASE("host transport mapper derives bar only from valid prerequisites",
          "[format][boundary][transport][validity]") {
    boundary::HostTransport transport;
    transport.position_beats = 8.0;
    transport.time_sig_numerator = 4;
    transport.time_sig_denominator = 4;

    SECTION("both prerequisites are valid") {
        transport.validity.set(TransportField::BeatPosition);
        transport.validity.set(TransportField::TimeSignature);
        ProcessContext ctx = base_context();
        detail::PlayheadSnapshot snapshot;
        boundary::apply_host_transport(ctx, transport, snapshot);
        CHECK(ctx.bar == 2);
        CHECK(ctx.has_transport(TransportField::Bar));
    }

    SECTION("beat position is unavailable") {
        transport.validity.set(TransportField::TimeSignature);
        ProcessContext ctx = base_context();
        detail::PlayheadSnapshot snapshot;
        boundary::apply_host_transport(ctx, transport, snapshot);
        CHECK_FALSE(ctx.has_transport(TransportField::Bar));
    }

    SECTION("time signature is unavailable") {
        transport.validity.set(TransportField::BeatPosition);
        ProcessContext ctx = base_context();
        detail::PlayheadSnapshot snapshot;
        boundary::apply_host_transport(ctx, transport, snapshot);
        CHECK_FALSE(ctx.has_transport(TransportField::Bar));
    }
}

TEST_CASE("empty host transport leaves compatibility defaults unavailable",
          "[format][boundary][transport][validity]") {
    ProcessContext ctx = base_context();
    detail::PlayheadSnapshot snapshot;
    boundary::apply_host_transport(ctx, {}, snapshot);

    CHECK(ctx.tempo_bpm == 120.0);
    CHECK(ctx.position_beats == 0.0);
    CHECK(ctx.position_samples == 0);
    CHECK(ctx.time_sig_numerator == 4);
    CHECK(ctx.time_sig_denominator == 4);
    CHECK(ctx.transport_validity.empty());
}

TEST_CASE("host transport mapper tracks validity acquisition and loss",
          "[format][boundary][transport][validity]") {
    detail::PlayheadSnapshot snapshot;

    ProcessContext absent = base_context();
    boundary::apply_host_transport(absent, {}, snapshot);
    CHECK_FALSE(absent.tempo_changed);

    boundary::HostTransport available_transport;
    available_transport.validity.set(TransportField::Tempo);
    available_transport.tempo_bpm = absent.tempo_bpm;
    ProcessContext acquired = base_context();
    boundary::apply_host_transport(acquired, available_transport, snapshot);
    CHECK(acquired.tempo_changed);

    ProcessContext lost = base_context();
    boundary::apply_host_transport(lost, {}, snapshot);
    CHECK(lost.tempo_changed);
    CHECK_FALSE(lost.transport_jump);
}
