#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/voice_modulation_buffer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;
using pulp::audio::VoiceModulationAudioRateReservation;
using pulp::audio::VoiceModulationBlock;
using pulp::audio::VoiceModulationBuffer;
using pulp::audio::VoiceModulationBufferConfig;
using pulp::audio::VoiceModulationRate;
using pulp::audio::VoiceModulationStatus;
using pulp::audio::VoiceModulationTarget;

namespace {

VoiceModulationTarget invalid_target() {
    return static_cast<VoiceModulationTarget>(255);
}

void require_status(bool ok,
                    VoiceModulationStatus actual,
                    VoiceModulationStatus expected) {
    REQUIRE(ok == (expected == VoiceModulationStatus::Ok));
    REQUIRE(actual == expected);
}

}  // namespace

TEST_CASE("VoiceModulationBuffer rejects unprepared hot operations",
          "[audio][sampler][voice-mod]") {
    VoiceModulationBuffer buffer;

    auto result = buffer.begin_block(4);
    require_status(result.ok, result.status, VoiceModulationStatus::NotPrepared);
    REQUIRE(buffer.block().empty());

    result = buffer.add_constant(VoiceModulationTarget::Gain, 0.5f);
    require_status(result.ok, result.status, VoiceModulationStatus::NotPrepared);

    std::array<float, 4> values{0.0f, 0.1f, 0.2f, 0.3f};
    result = buffer.add_audio_rate(VoiceModulationTarget::PitchCents, values);
    require_status(result.ok, result.status, VoiceModulationStatus::NotPrepared);

    const auto reservation = buffer.reserve_audio_rate(VoiceModulationTarget::Pan);
    REQUIRE_FALSE(reservation.ok);
    REQUIRE(reservation.status == VoiceModulationStatus::NotPrepared);
    REQUIRE(reservation.values == nullptr);
}

TEST_CASE("VoiceModulationBuffer prepares, begins blocks, and releases storage",
          "[audio][sampler][voice-mod]") {
    VoiceModulationBuffer buffer;

    REQUIRE_FALSE(buffer.prepare(VoiceModulationBufferConfig{
        .max_lanes = 0,
        .max_frames = 8,
    }));
    REQUIRE_FALSE(buffer.prepared());

    REQUIRE(buffer.prepare(VoiceModulationBufferConfig{
        .max_lanes = 2,
        .max_frames = 4,
    }));
    REQUIRE(buffer.prepared());
    REQUIRE(buffer.max_lanes() == 2);
    REQUIRE(buffer.max_frames() == 4);

    auto result = buffer.begin_block(0);
    require_status(result.ok, result.status, VoiceModulationStatus::InvalidFrameCount);
    REQUIRE(buffer.block().empty());

    result = buffer.begin_block(5);
    require_status(result.ok, result.status, VoiceModulationStatus::InvalidFrameCount);
    REQUIRE(buffer.block().empty());

    result = buffer.begin_block(4);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);
    REQUIRE(buffer.frame_count() == 4);
    REQUIRE(buffer.block().empty());
    REQUIRE(buffer.block().frame_count == 4);

    buffer.release();
    REQUIRE_FALSE(buffer.prepared());
    REQUIRE(buffer.block().empty());
}

TEST_CASE("VoiceModulationBuffer stores constant lanes and rejects duplicates",
          "[audio][sampler][voice-mod]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 4, .max_frames = 8}));
    auto result = buffer.begin_block(8);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    result = buffer.add_constant(VoiceModulationTarget::Gain, 0.75f);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    result = buffer.add_constant(VoiceModulationTarget::Gain, 0.5f);
    require_status(result.ok, result.status, VoiceModulationStatus::DuplicateTarget);
    REQUIRE(buffer.overflow_count() == 0);

    const auto block = buffer.block();
    REQUIRE(block.frame_count == 8);
    REQUIRE(block.lanes.size() == 1);
    REQUIRE(block.lanes[0].rate == VoiceModulationRate::Constant);
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::Gain, 0),
                 WithinAbs(0.75f, 1.0e-6f));
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::Gain, 99),
                 WithinAbs(0.75f, 1.0e-6f));
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::Pan, 0, -1.0f),
                 WithinAbs(-1.0f, 1.0e-6f));
}

TEST_CASE("VoiceModulationBuffer copies audio-rate lanes and clamps reads",
          "[audio][sampler][voice-mod]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 4}));
    auto result = buffer.begin_block(4);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    std::array<float, 4> values{0.0f, 0.25f, 0.5f, 1.0f};
    result = buffer.add_audio_rate(VoiceModulationTarget::PitchCents, values);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);
    values[1] = 999.0f;

    const auto block = buffer.block();
    REQUIRE(block.lanes.size() == 1);
    REQUIRE(block.lanes[0].rate == VoiceModulationRate::AudioRate);
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::PitchCents, 0),
                 WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::PitchCents, 1),
                 WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::PitchCents, 99),
                 WithinAbs(1.0f, 1.0e-6f));
}

TEST_CASE("VoiceModulationBuffer rejects invalid targets and non-finite values",
          "[audio][sampler][voice-mod][edge]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 4, .max_frames = 4}));
    auto result = buffer.begin_block(4);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    result = buffer.add_constant(invalid_target(), 1.0f);
    require_status(result.ok, result.status, VoiceModulationStatus::InvalidTarget);

    result = buffer.add_constant(VoiceModulationTarget::Gain, std::nanf(""));
    require_status(result.ok, result.status, VoiceModulationStatus::NonFiniteValue);

    std::array<float, 4> values{
        0.0f,
        0.1f,
        std::numeric_limits<float>::infinity(),
        0.3f,
    };
    result = buffer.add_audio_rate(VoiceModulationTarget::Pressure, values);
    require_status(result.ok, result.status, VoiceModulationStatus::NonFiniteValue);
    REQUIRE(buffer.block().empty());
}

TEST_CASE("VoiceModulationBuffer reports lane overflow without mutating lanes",
          "[audio][sampler][voice-mod][edge]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 1, .max_frames = 4}));
    auto result = buffer.begin_block(4);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    result = buffer.add_constant(VoiceModulationTarget::Gain, 1.0f);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    result = buffer.add_constant(VoiceModulationTarget::Pan, 0.0f);
    require_status(result.ok, result.status, VoiceModulationStatus::LaneOverflow);
    REQUIRE(buffer.overflow_count() == 1);
    REQUIRE(buffer.block().lanes.size() == 1);
    REQUIRE(buffer.block().find(VoiceModulationTarget::Pan) == nullptr);

    result = buffer.begin_block(4);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);
    REQUIRE(buffer.overflow_count() == 1);
    REQUIRE(buffer.block().empty());
    REQUIRE(buffer.block().frame_count == 4);

    buffer.reset();
    REQUIRE(buffer.overflow_count() == 0);
    REQUIRE(buffer.block().empty());
    REQUIRE(buffer.block().frame_count == 0);
}

TEST_CASE("VoiceModulationBuffer reserves zero-filled audio-rate lanes",
          "[audio][sampler][voice-mod]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 3}));
    auto result = buffer.begin_block(3);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    VoiceModulationAudioRateReservation reservation =
        buffer.reserve_audio_rate(VoiceModulationTarget::Timbre);
    REQUIRE(reservation.ok);
    REQUIRE(reservation.status == VoiceModulationStatus::Ok);
    REQUIRE(reservation.values != nullptr);
    REQUIRE(reservation.frame_count == 3);
    REQUIRE_THAT(reservation.values[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(reservation.values[1], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(reservation.values[2], WithinAbs(0.0f, 1.0e-6f));

    reservation.values[0] = 0.25f;
    reservation.values[1] = 0.5f;
    reservation.values[2] = 0.75f;

    const auto duplicate = buffer.reserve_audio_rate(VoiceModulationTarget::Timbre);
    REQUIRE_FALSE(duplicate.ok);
    REQUIRE(duplicate.status == VoiceModulationStatus::DuplicateTarget);

    const auto block = buffer.block();
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::Timbre, 0),
                 WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::Timbre, 2),
                 WithinAbs(0.75f, 1.0e-6f));
}

TEST_CASE("VoiceModulationBuffer clears stale block state on begin failures",
          "[audio][sampler][voice-mod][edge]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 3}));
    auto result = buffer.begin_block(3);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);

    result = buffer.add_constant(VoiceModulationTarget::Gain, 1.0f);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);
    REQUIRE_FALSE(buffer.block().empty());

    result = buffer.begin_block(4);
    require_status(result.ok, result.status, VoiceModulationStatus::InvalidFrameCount);
    REQUIRE(buffer.block().empty());

    result = buffer.begin_block(3);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);
    auto reservation = buffer.reserve_audio_rate(VoiceModulationTarget::Aux0);
    REQUIRE(reservation.ok);
    reservation.values[0] = 9.0f;
    reservation.values[1] = 9.0f;
    reservation.values[2] = 9.0f;

    result = buffer.begin_block(3);
    require_status(result.ok, result.status, VoiceModulationStatus::Ok);
    reservation = buffer.reserve_audio_rate(VoiceModulationTarget::Aux0);
    REQUIRE(reservation.ok);
    REQUIRE_THAT(reservation.values[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(reservation.values[1], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(reservation.values[2], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("VoiceModulationBuffer hot block path does not allocate",
          "[audio][sampler][voice-mod][rt]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 4, .max_frames = 8}));
    std::array<float, 8> pitch{};
    for (std::size_t i = 0; i < pitch.size(); ++i) {
        pitch[i] = static_cast<float>(i);
    }

    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        auto result = buffer.begin_block(8);
        REQUIRE(result.ok);
        result = buffer.add_constant(VoiceModulationTarget::Gain, 0.5f);
        REQUIRE(result.ok);
        result = buffer.add_audio_rate(VoiceModulationTarget::PitchCents, pitch);
        REQUIRE(result.ok);
        const auto reservation =
            buffer.reserve_audio_rate(VoiceModulationTarget::Pressure);
        REQUIRE(reservation.ok);
        reservation.values[0] = 1.0f;
    }

    const auto block = buffer.block();
    REQUIRE(block.lanes.size() == 3);
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::Pressure, 0),
                 WithinAbs(1.0f, 1.0e-6f));
}
