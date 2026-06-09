#include <catch2/catch_test_macros.hpp>

#include <pulp/format/transport_quantizer.hpp>

#include <cmath>
#include <limits>

using pulp::format::ProcessContext;
using pulp::format::TransportQuantizePolicy;
using pulp::format::TransportQuantizeRequest;
using pulp::format::TransportQuantizeStatus;
using pulp::format::TransportQuantizer;

namespace {

ProcessContext playing_context() {
    ProcessContext context;
    context.sample_rate = 48000.0;
    context.num_samples = 512;
    context.is_playing = true;
    context.tempo_bpm = 120.0;
    context.time_sig_numerator = 4;
    context.time_sig_denominator = 4;
    return context;
}

TransportQuantizeRequest request(TransportQuantizePolicy policy) {
    TransportQuantizeRequest request;
    request.policy = policy;
    return request;
}

}  // namespace

TEST_CASE("TransportQuantizer schedules immediate requests at block start",
          "[format][transport][quantizer]") {
    ProcessContext context;
    context.is_playing = false;

    TransportQuantizeRequest immediate;
    immediate.policy = TransportQuantizePolicy::Immediate;
    immediate.require_playing = false;

    TransportQuantizer quantizer;
    const auto block = quantizer.begin_block(context);
    const auto result = quantizer.resolve(context, immediate, block);

    REQUIRE(result.scheduled);
    REQUIRE(result.block_offset == 0);
    REQUIRE(result.status == TransportQuantizeStatus::Scheduled);
}

TEST_CASE("TransportQuantizer maps next beat to a block-relative offset",
          "[format][transport][quantizer]") {
    auto context = playing_context();
    context.position_beats = 3.999;

    TransportQuantizer quantizer;
    const auto block = quantizer.begin_block(context);
    const auto result = quantizer.resolve(context,
                                         request(TransportQuantizePolicy::NextBeat),
                                         block);

    REQUIRE(result.scheduled);
    REQUIRE(result.block_offset == 24);
    REQUIRE(std::abs(result.target_beats - 4.0) < 1.0e-12);
}

TEST_CASE("TransportQuantizer maps offsets across common host sample rates",
          "[format][transport][quantizer]") {
    const double sample_rates[] = {
        44100.0,
        48000.0,
        88200.0,
        96000.0,
        176400.0,
        192000.0,
    };

    for (const auto sample_rate : sample_rates) {
        auto context = playing_context();
        context.sample_rate = sample_rate;
        context.position_beats = 3.98;
        context.num_samples = static_cast<int>(std::llround(sample_rate * 0.02));

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context,
                                             request(TransportQuantizePolicy::NextBeat),
                                             block);

        INFO("sample_rate=" << sample_rate);
        REQUIRE(result.scheduled);
        REQUIRE(result.block_offset ==
                static_cast<std::uint32_t>(std::llround(sample_rate * 0.01)));
        REQUIRE(std::abs(result.target_beats - 4.0) < 1.0e-12);
    }
}

TEST_CASE("TransportQuantizer block plan binds the matching context and block",
          "[format][transport][quantizer]") {
    auto context = playing_context();
    context.position_beats = 3.999;

    TransportQuantizer quantizer;
    const auto plan = quantizer.begin_block_plan(context);
    context.position_beats = 0.0;
    context.num_samples = 1;

    const auto result = plan.resolve(request(TransportQuantizePolicy::NextBeat));
    REQUIRE(result.scheduled);
    REQUIRE(result.block_offset == 24);
    REQUIRE(std::abs(result.target_beats - 4.0) < 1.0e-12);
    REQUIRE(std::abs(plan.context().position_beats - 3.999) < 1.0e-12);
    REQUIRE_FALSE(plan.transport_jumped());
    REQUIRE(plan.timeline_valid());
}

TEST_CASE("TransportQuantizer maps next bars for common meters",
          "[format][transport][quantizer]") {
    SECTION("4/4") {
        auto context = playing_context();
        context.position_beats = 3.99;

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context,
                                             request(TransportQuantizePolicy::NextBar),
                                             block);

        REQUIRE(result.scheduled);
        REQUIRE(result.block_offset == 240);
        REQUIRE(std::abs(result.target_beats - 4.0) < 1.0e-12);
    }

    SECTION("6/8") {
        auto context = playing_context();
        context.time_sig_numerator = 6;
        context.time_sig_denominator = 8;
        context.position_beats = 2.99;

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context,
                                             request(TransportQuantizePolicy::NextBar),
                                             block);

        REQUIRE(result.scheduled);
        REQUIRE(result.block_offset == 240);
        REQUIRE(std::abs(result.target_beats - 3.0) < 1.0e-12);
    }
}

TEST_CASE("TransportQuantizer supports arbitrary grid sizes",
          "[format][transport][quantizer]") {
    auto context = playing_context();
    context.position_beats = 7.74;

    auto next_sixteenth = request(TransportQuantizePolicy::NextGrid);
    next_sixteenth.grid_beats = 0.25;

    TransportQuantizer quantizer;
    const auto block = quantizer.begin_block(context);
    const auto result = quantizer.resolve(context, next_sixteenth, block);

    REQUIRE(result.scheduled);
    REQUIRE(result.block_offset == 240);
    REQUIRE(std::abs(result.target_beats - 7.75) < 1.0e-12);
}

TEST_CASE("TransportQuantizer reports boundaries outside the current block",
          "[format][transport][quantizer]") {
    auto context = playing_context();
    context.position_beats = 3.5;
    context.num_samples = 128;

    TransportQuantizer quantizer;
    const auto block = quantizer.begin_block(context);
    const auto result = quantizer.resolve(context,
                                         request(TransportQuantizePolicy::NextBeat),
                                         block);

    REQUIRE_FALSE(result.scheduled);
    REQUIRE(result.status == TransportQuantizeStatus::OutsideBlock);
}

TEST_CASE("TransportQuantizer handles stopped and invalid transport data",
          "[format][transport][quantizer]") {
    SECTION("stopped transport") {
        auto context = playing_context();
        context.is_playing = false;

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context,
                                             request(TransportQuantizePolicy::NextBeat),
                                             block);

        REQUIRE_FALSE(result.scheduled);
        REQUIRE(result.status == TransportQuantizeStatus::TransportStopped);
    }

    SECTION("invalid tempo") {
        auto context = playing_context();
        context.tempo_bpm = 0.0;

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context,
                                             request(TransportQuantizePolicy::NextBeat),
                                             block);

        REQUIRE_FALSE(result.scheduled);
        REQUIRE(result.status == TransportQuantizeStatus::InvalidTempo);
    }

    SECTION("invalid sample rate") {
        auto context = playing_context();
        context.sample_rate = 0.0;

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context,
                                             request(TransportQuantizePolicy::NextBeat),
                                             block);

        REQUIRE_FALSE(result.scheduled);
        REQUIRE(result.status == TransportQuantizeStatus::InvalidSampleRate);
    }

    SECTION("invalid grid") {
        auto context = playing_context();
        auto grid = request(TransportQuantizePolicy::NextGrid);
        grid.grid_beats = 0.0;

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context, grid, block);

        REQUIRE_FALSE(result.scheduled);
        REQUIRE(result.status == TransportQuantizeStatus::InvalidGrid);
    }

    SECTION("invalid timeline position") {
        auto context = playing_context();
        context.position_beats = std::numeric_limits<double>::quiet_NaN();

        TransportQuantizer quantizer;
        const auto block = quantizer.begin_block(context);
        const auto result = quantizer.resolve(context,
                                             request(TransportQuantizePolicy::NextBeat),
                                             block);

        REQUIRE_FALSE(result.scheduled);
        REQUIRE(result.status == TransportQuantizeStatus::InvalidTimeline);
    }
}

TEST_CASE("TransportQuantizer detects transport jumps across blocks",
          "[format][transport][quantizer]") {
    auto first = playing_context();
    first.position_beats = 1.0;
    first.num_samples = 128;

    auto jumped = playing_context();
    jumped.position_beats = 8.0;
    jumped.num_samples = 128;

    TransportQuantizer quantizer;
    auto block = quantizer.begin_block(first);
    REQUIRE_FALSE(block.transport_jumped);

    block = quantizer.begin_block(jumped);
    REQUIRE(block.transport_jumped);

    auto next = request(TransportQuantizePolicy::NextBeat);
    auto result = quantizer.resolve(jumped, next, block);
    REQUIRE_FALSE(result.scheduled);
    REQUIRE(result.status == TransportQuantizeStatus::TransportJumped);

    next.cancel_on_transport_jump = false;
    result = quantizer.resolve(jumped, next, block);
    REQUIRE(result.scheduled);
    REQUIRE(result.transport_jumped);
}

TEST_CASE("TransportQuantizer block plan carries transport jump metadata",
          "[format][transport][quantizer]") {
    auto first = playing_context();
    first.position_beats = 1.0;
    first.num_samples = 128;

    auto jumped = playing_context();
    jumped.position_beats = 8.0;
    jumped.num_samples = 128;

    TransportQuantizer quantizer;
    auto first_plan = quantizer.begin_block_plan(first);
    REQUIRE_FALSE(first_plan.transport_jumped());

    auto jump_plan = quantizer.begin_block_plan(jumped);
    REQUIRE(jump_plan.transport_jumped());

    auto next = request(TransportQuantizePolicy::NextBeat);
    auto result = jump_plan.resolve(next);
    REQUIRE_FALSE(result.scheduled);
    REQUIRE(result.status == TransportQuantizeStatus::TransportJumped);

    next.cancel_on_transport_jump = false;
    result = jump_plan.resolve(next);
    REQUIRE(result.scheduled);
    REQUIRE(result.transport_jumped);
}

TEST_CASE("TransportQuantizer does not treat host loop wraps as jumps",
          "[format][transport][quantizer]") {
    auto first = playing_context();
    first.position_beats = 7.99;
    first.num_samples = 480;
    first.is_looping = true;
    first.loop_start_beats = 4.0;
    first.loop_end_beats = 8.0;

    auto wrapped = first;
    wrapped.position_beats = 4.01;

    TransportQuantizer quantizer;
    auto block = quantizer.begin_block(first);
    REQUIRE_FALSE(block.transport_jumped);
    block = quantizer.begin_block(wrapped);
    REQUIRE_FALSE(block.transport_jumped);
}

TEST_CASE("TransportQuantizer schedules host loop-start wrap boundaries",
          "[format][transport][quantizer]") {
    auto context = playing_context();
    context.position_beats = 7.99;
    context.is_looping = true;
    context.loop_start_beats = 4.0;
    context.loop_end_beats = 8.0;

    TransportQuantizer quantizer;
    const auto block = quantizer.begin_block(context);
    const auto result = quantizer.resolve(context,
                                         request(TransportQuantizePolicy::HostLoopStart),
                                         block);

    REQUIRE(result.scheduled);
    REQUIRE(result.block_offset == 240);
    REQUIRE(std::abs(result.target_beats - 8.0) < 1.0e-12);
}

TEST_CASE("TransportQuantizer tolerates variable block sizes on a steady timeline",
          "[format][transport][quantizer]") {
    TransportQuantizer quantizer;

    double position_beats = 2.0;
    const int block_sizes[] = {1, 16, 64, 128, 512, 1024};
    for (const auto block_size : block_sizes) {
        auto context = playing_context();
        context.position_beats = position_beats;
        context.num_samples = block_size;
        INFO("block_size=" << block_size);
        const auto block = quantizer.begin_block(context);
        REQUIRE_FALSE(block.transport_jumped);
        position_beats += (static_cast<double>(block_size) / context.sample_rate) *
                          (context.tempo_bpm / 60.0);
    }
}
