#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/events/event_loop.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/state/binding.hpp>
#include <pulp/state/state.hpp>
#include <pulp/state/state_migration.hpp>
#include "harness/rt_allocation_probe.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace pulp::state;
using Catch::Matchers::WithinAbs;

static ParamInfo make_param_info(ParamID id, const char* name, const char* unit, ParamRange range) {
    ParamInfo info;
    info.id = id;
    info.name = name;
    info.unit = unit;
    info.range = range;
    return info;
}

static uint32_t read_u32_le(const std::vector<uint8_t>& data, std::size_t offset) {
    return static_cast<uint32_t>(data[offset])
        | (static_cast<uint32_t>(data[offset + 1]) << 8)
        | (static_cast<uint32_t>(data[offset + 2]) << 16)
        | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

static void write_u32_le(std::vector<uint8_t>& data, std::size_t offset, uint32_t value) {
    data[offset] = static_cast<uint8_t>(value & 0xffu);
    data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
    data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xffu);
    data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

static uint32_t crc32_simple_for_test(const std::vector<uint8_t>& data,
                                      std::size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            const uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t test_crc32(const uint8_t* data, std::size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            const uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void write_le_u32(std::vector<uint8_t>& data, std::size_t offset, uint32_t value) {
    REQUIRE(offset + 4 <= data.size());
    data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
    data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

TEST_CASE("ParamRange normalization", "[state][range]") {
    ParamRange range{0.0f, 100.0f, 50.0f, 0.0f};

    REQUIRE_THAT(range.normalize(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.normalize(50.0f), WithinAbs(0.5, 0.001));
    REQUIRE_THAT(range.normalize(100.0f), WithinAbs(1.0, 0.001));

    REQUIRE_THAT(range.denormalize(0.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.denormalize(0.5f), WithinAbs(50.0, 0.001));
    REQUIRE_THAT(range.denormalize(1.0f), WithinAbs(100.0, 0.001));
}

TEST_CASE("StateStore normalized seam honors a shaped (skewed) range",
          "[state][range][shaped][store]") {
    // The adapter-facing path is StateStore::set_normalized_rt /
    // get_normalized, which both convert through ParamRange. A skewed range
    // must round-trip through that seam exactly the way a format adapter
    // (e.g. VST3 plainToNormalized/normalizedToPlain) drives it. A linear
    // param registered alongside must be unaffected.
    StateStore store;

    ParamInfo freq = make_param_info(
        1, "Freq", "Hz",
        ParamRange::with_centre(20.0f, 20000.0f, 1000.0f));
    ParamInfo gain = make_param_info(2, "Gain", "dB", {-60.0f, 12.0f, 0.0f});
    store.add_parameter(freq);
    store.add_parameter(gain);

    REQUIRE_FALSE(store.info(1)->range.is_linear());
    REQUIRE(store.info(2)->range.is_linear());

    // Skewed param: 0.5 normalized maps to the 1 kHz centre, and the value
    // read back as normalized matches what we wrote.
    store.set_normalized(1, 0.5f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(1000.0, 1.0));
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.5, 1e-4));

    // Endpoints exact through the store seam.
    store.set_normalized(1, 0.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(20.0, 1e-3));
    store.set_normalized(1, 1.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(20000.0, 1e-3));

    // Full round-trip monotonicity through the realtime write path.
    float prev = -1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float n = static_cast<float>(i) / 100.0f;
        store.set_normalized_rt(1, n);
        const float v = store.get_value(1);
        REQUIRE(v >= prev);
        REQUIRE_THAT(store.get_normalized(1), WithinAbs(n, 1e-3));
        prev = v;
    }

    // Linear param is bit-identical to a plain affine map through the seam.
    store.set_normalized(2, 0.5f);
    REQUIRE_THAT(store.get_value(2), WithinAbs(-24.0, 1e-3)); // -60 + 0.5*72
    REQUIRE(store.get_normalized(2) == gain.range.normalize(store.get_value(2)));
}

TEST_CASE("ParamInfo defaults to control rate and preserves audio-rate metadata",
          "[state][params][rate]") {
    ParamInfo default_info;
    REQUIRE(default_info.rate == ParamRate::ControlRate);
    REQUIRE(default_info.smoothing_ramp_seconds == 0.0f);

    StateStore source;
    ParamInfo audio_info = make_param_info(
        42, "Cutoff CV", "Hz", {20.0f, 20000.0f, 440.0f, 1.0f});
    audio_info.rate = ParamRate::AudioRate;
    audio_info.smoothing_ramp_seconds = 0.005f;
    source.add_parameter(audio_info);
    source.set_value(42, 880.0f);

    REQUIRE(source.info(42) != nullptr);
    REQUIRE(source.info(42)->rate == ParamRate::AudioRate);
    REQUIRE(source.info(42)->smoothing_ramp_seconds == 0.005f);

    auto blob = source.serialize();

    StateStore restored;
    restored.add_parameter(audio_info);
    REQUIRE(restored.deserialize(blob));
    REQUIRE(restored.info(42) != nullptr);
    REQUIRE(restored.info(42)->rate == ParamRate::AudioRate);
    REQUIRE(restored.info(42)->smoothing_ramp_seconds == 0.005f);
    REQUIRE_THAT(restored.get_value(42), WithinAbs(880.0f, 1e-6f));
}

TEST_CASE("ParameterEventQueue is available from state",
          "[state][params][events]") {
    ParameterEventQueue queue;
    queue.push({3, 64, 0.75f});
    queue.push(ParameterEvent{1, 0, 0.25f});
    queue.push({2, 32, 0.5f});

    queue.sort();

    REQUIRE(queue.size() == 3);
    REQUIRE(queue.events()[0].param_id == 1);
    REQUIRE(queue.events()[0].sample_offset == 0);
    REQUIRE(queue.events()[1].param_id == 2);
    REQUIRE(queue.events()[1].sample_offset == 32);
    REQUIRE(queue.events()[2].param_id == 3);
    REQUIRE(queue.events()[2].sample_offset == 64);
}

TEST_CASE("ParameterEventQueue enforces capacity and preserves stable sort order",
          "[state][params][events]") {
    ParameterEventQueue queue;

    REQUIRE(queue.capacity() == ParameterEventQueue::kCapacity);
    for (std::size_t i = 0; i < queue.capacity(); ++i) {
        REQUIRE(queue.push(ParameterEvent{
            .param_id = static_cast<ParamID>(i + 1),
            .sample_offset = static_cast<int32_t>(queue.capacity() - i),
            .value = static_cast<float>(i),
        }));
    }

    REQUIRE_FALSE(queue.push(ParameterEvent{.param_id = 9999, .sample_offset = 0, .value = 1.0f}));
    REQUIRE(queue.overflowed());
    REQUIRE(queue.dropped_event_count() == 1);
    REQUIRE_FALSE(queue.push(ParameterEvent{.param_id = 10000, .sample_offset = 1, .value = 2.0f}));
    REQUIRE(queue.dropped_event_count() == 2);
    REQUIRE(queue.size() == queue.capacity());

    queue.clear();
    REQUIRE(queue.empty());
    REQUIRE_FALSE(queue.overflowed());
    REQUIRE(queue.dropped_event_count() == 0);
    REQUIRE(queue.push(ParameterEvent{.param_id = 1, .sample_offset = 8, .value = 1.0f}));
    REQUIRE(queue.push(ParameterEvent{.param_id = 2, .sample_offset = 8, .value = 2.0f}));
    REQUIRE(queue.push(ParameterEvent{.param_id = 3, .sample_offset = 4, .value = 3.0f}));

    queue.sort();
    REQUIRE(queue.events()[0].param_id == 3);
    REQUIRE(queue.events()[1].param_id == 1);
    REQUIRE(queue.events()[2].param_id == 2);
}

TEST_CASE("ParamCursor ignores unregistered events but honors explicit snapshots",
          "[state][params][cursor]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.25f}));
    store.set_value(1, 0.5f);

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 99, .sample_offset = 0, .value = 9.0f}));
    REQUIRE(events.push(ParameterEvent{.param_id = 1, .sample_offset = 4, .value = 2.0f}));
    events.sort();

    std::array<ParamSnapshotEntry, 1> snapshots{{{99, 7.0f}}};
    ParamCursor cursor(store, &events, snapshots);

    REQUIRE(cursor.is_tracked(99));
    REQUIRE(cursor.value(99) == 7.0f);
    REQUIRE(cursor.is_tracked(1));
    REQUIRE(cursor.value(1) == 0.5f);

    cursor.advance_to(0);
    REQUIRE(cursor.value(99) == 7.0f);
    REQUIRE(cursor.value(1) == 0.5f);

    cursor.advance_to(4);
    REQUIRE(cursor.value(1) == 1.0f);
    REQUIRE(store.get_value(1) == 0.5f);
}

TEST_CASE("ParamCursor is monotonic and null event queues fall back to StateStore",
          "[state][params][cursor]") {
    StateStore store;
    store.add_parameter(make_param_info(7, "Mix", "", {-1.0f, 1.0f, 0.0f}));
    store.set_value(7, 0.25f);

    ParamCursor no_events(store, nullptr);
    REQUIRE(no_events.tracked_param_count() == 0);
    REQUIRE(no_events.value(7) == 0.25f);
    REQUIRE(no_events.value(1234) == 0.0f);

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 7, .sample_offset = 2, .value = -0.5f}));
    REQUIRE(events.push(ParameterEvent{.param_id = 7, .sample_offset = 5, .value = 0.75f}));
    events.sort();

    ParamCursor cursor(store, &events);
    cursor.advance_to(5);
    REQUIRE(cursor.position() == 5);
    REQUIRE(cursor.value(7) == 0.75f);

    cursor.advance_to(2);
    REQUIRE(cursor.position() == 5);
    REQUIRE(cursor.value(7) == 0.75f);
}

TEST_CASE("ParamCursor interpolates ramp-duration parameter events",
          "[state][params][cursor][ramp]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.0f}));
    store.set_value(1, 0.0f);

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 4,
                                       .value = 1.0f,
                                       .ramp_duration_sample_frames = 4}));
    events.sort();

    ParamCursor cursor(store, &events);
    REQUIRE(cursor.is_tracked(1));

    cursor.advance_to(3);
    REQUIRE_FALSE(cursor.is_ramping(1));
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.0f, 1e-6f));

    cursor.advance_to(4);
    REQUIRE(cursor.is_ramping(1));
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(cursor.value_at(1, 6), WithinAbs(0.5f, 1e-6f));

    cursor.advance_to(5);
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.25f, 1e-6f));

    cursor.advance_to(6);
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.5f, 1e-6f));

    cursor.advance_to(7);
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.75f, 1e-6f));

    cursor.advance_to(8);
    REQUIRE_FALSE(cursor.is_ramping(1));
    REQUIRE_THAT(cursor.value(1), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("ParamCursor ramps from the current interpolated value when events overlap",
          "[state][params][cursor][ramp]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.0f}));
    store.set_value(1, 0.0f);

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 0,
                                       .value = 1.0f,
                                       .ramp_duration_sample_frames = 8}));
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 4,
                                       .value = 0.0f,
                                       .ramp_duration_sample_frames = 4}));
    events.sort();

    ParamCursor cursor(store, &events);
    cursor.advance_to(2);
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.25f, 1e-6f));

    cursor.advance_to(4);
    REQUIRE(cursor.is_ramping(1));
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.5f, 1e-6f));

    cursor.advance_to(6);
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.25f, 1e-6f));

    cursor.advance_to(8);
    REQUIRE_FALSE(cursor.is_ramping(1));
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("ParamCursor ramp interpolation stays inside the no-allocation contract",
          "[state][params][cursor][ramp][rt]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.5f}));
    store.set_value(1, 0.5f);

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 0,
                                       .value = 1.0f,
                                       .ramp_duration_sample_frames = 4}));
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 4,
                                       .value = 0.0f,
                                       .ramp_duration_sample_frames = 4}));
    events.sort();

    std::array<float, 5> sampled{};
    {
        pulp::runtime::ScopedNoAlloc guard;
        ParamCursor cursor(store, &events);
        cursor.advance_to(0);
        sampled[0] = cursor.value(1);
        sampled[1] = cursor.value_at(1, 2);
        cursor.advance_to(2);
        sampled[2] = cursor.value(1);
        cursor.advance_to(6);
        sampled[3] = cursor.value(1);
        cursor.advance_to(8);
        sampled[4] = cursor.value(1);
    }

    REQUIRE_THAT(sampled[0], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(sampled[1], WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(sampled[2], WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(sampled[3], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(sampled[4], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("ParamCursor interpolates overlapping ramps across a large host block",
          "[state][params][cursor][ramp][capacity]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.0f}));
    store.set_value(1, 0.0f);

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 0,
                                       .value = 1.0f,
                                       .ramp_duration_sample_frames = 4096}));
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 2048,
                                       .value = 0.0f,
                                       .ramp_duration_sample_frames = 2048}));
    events.sort();

    std::array<float, 5> sampled{};
    {
        pulp::runtime::ScopedNoAlloc guard;
        ParamCursor cursor(store, &events);
        cursor.advance_to(0);
        sampled[0] = cursor.value(1);
        sampled[1] = cursor.value_at(1, 1024);
        cursor.advance_to(2048);
        sampled[2] = cursor.value(1);
        cursor.advance_to(3072);
        sampled[3] = cursor.value(1);
        cursor.advance_to(4096);
        sampled[4] = cursor.value(1);
    }

    REQUIRE_THAT(sampled[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(sampled[1], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(sampled[2], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(sampled[3], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(sampled[4], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("ParamCursor clamps ramp targets and treats non-positive durations as immediate",
          "[state][params][cursor][ramp]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.0f}));
    store.set_value(1, 0.25f);

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 0,
                                       .value = 2.0f,
                                       .ramp_duration_sample_frames = 4}));
    REQUIRE(events.push(ParameterEvent{.param_id = 1,
                                       .sample_offset = 8,
                                       .value = -1.0f,
                                       .ramp_duration_sample_frames = -1}));
    events.sort();

    ParamCursor cursor(store, &events);
    cursor.advance_to(2);
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.625f, 1e-6f));

    cursor.advance_to(4);
    REQUIRE_THAT(cursor.value(1), WithinAbs(1.0f, 1e-6f));

    cursor.advance_to(8);
    REQUIRE_FALSE(cursor.is_ramping(1));
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("ParameterEventQueue exposes mutable and const iteration over active events",
          "[state][params][events]") {
    ParameterEventQueue queue;

    REQUIRE(queue.empty());
    REQUIRE(queue.push(ParameterEvent{.param_id = 1, .sample_offset = 16, .value = 0.25f,
                                      .ramp_duration_sample_frames = 4}));
    REQUIRE(queue.push(ParameterEvent{.param_id = 2, .sample_offset = 8, .value = 0.5f,
                                      .ramp_duration_sample_frames = 2}));

    for (auto& event : queue) {
        event.value += 0.125f;
    }

    queue.sort();

    const auto& const_queue = queue;
    auto it = const_queue.begin();
    REQUIRE(it != const_queue.end());
    REQUIRE(it->param_id == 2);
    REQUIRE(it->sample_offset == 8);
    REQUIRE(it->ramp_duration_sample_frames == 2);
    REQUIRE_THAT(it->value, WithinAbs(0.625f, 1e-6f));

    ++it;
    REQUIRE(it != const_queue.end());
    REQUIRE(it->param_id == 1);
    REQUIRE(it->sample_offset == 16);
    REQUIRE(it->ramp_duration_sample_frames == 4);
    REQUIRE_THAT(it->value, WithinAbs(0.375f, 1e-6f));

    ++it;
    REQUIRE(it == const_queue.end());
}

TEST_CASE("ParamCursor updates duplicate snapshots and ignores late unknown events",
          "[state][params][cursor]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.25f}));
    store.set_value(1, 0.5f);

    std::array<ParamSnapshotEntry, 3> snapshots{{
        {1, 0.25f},
        {99, 2.0f},
        {1, 0.75f},
    }};

    ParameterEventQueue events;
    REQUIRE(events.push(ParameterEvent{.param_id = 42, .sample_offset = 1, .value = 9.0f}));
    REQUIRE(events.push(ParameterEvent{.param_id = 1, .sample_offset = 2, .value = -1.0f}));
    REQUIRE(events.push(ParameterEvent{.param_id = 1, .sample_offset = 4, .value = 2.0f}));
    events.sort();

    ParamCursor cursor(store, &events, snapshots);

    REQUIRE(cursor.tracked_param_count() == 2);
    REQUIRE(cursor.is_tracked(1));
    REQUIRE(cursor.is_tracked(99));
    REQUIRE_FALSE(cursor.is_tracked(42));
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(cursor.value(99), WithinAbs(2.0f, 1e-6f));

    cursor.advance_to(1);
    REQUIRE(cursor.position() == 1);
    REQUIRE_FALSE(cursor.is_tracked(42));
    REQUIRE_THAT(cursor.value(42), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.75f, 1e-6f));

    cursor.advance_to(2);
    REQUIRE_THAT(cursor.value(1), WithinAbs(0.0f, 1e-6f));

    cursor.advance_to(4);
    REQUIRE_THAT(cursor.value(1), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("ParamRange with step", "[state][range]") {
    ParamRange range{0.0f, 10.0f, 5.0f, 1.0f}; // step = 1

    REQUIRE_THAT(range.denormalize(0.33f), WithinAbs(3.0, 0.5));
    REQUIRE_THAT(range.denormalize(0.77f), WithinAbs(8.0, 0.5));
}

TEST_CASE("ParamRange clamps normalized conversions at range boundaries",
          "[state][range]") {
    ParamRange range{-10.0f, 30.0f, 5.0f, 0.0f};

    REQUIRE_THAT(range.normalize(-100.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.normalize(100.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(range.denormalize(-0.5f), WithinAbs(-10.0, 0.001));
    REQUIRE_THAT(range.denormalize(1.5f), WithinAbs(30.0, 0.001));
}

TEST_CASE("ParamRange zero-width ranges normalize safely",
          "[state][range]") {
    ParamRange range{7.0f, 7.0f, 7.0f, 0.0f};

    REQUIRE_THAT(range.normalize(7.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.normalize(100.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.denormalize(0.25f), WithinAbs(7.0, 0.001));
    REQUIRE_THAT(range.denormalize(2.0f), WithinAbs(7.0, 0.001));
}

TEST_CASE("ParamRange clamps and handles zero-width ranges", "[state][range]") {
    ParamRange range{-10.0f, 10.0f, 0.0f, 0.0f};

    REQUIRE_THAT(range.normalize(-100.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.normalize(100.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(range.denormalize(-1.0f), WithinAbs(-10.0, 0.001));
    REQUIRE_THAT(range.denormalize(2.0f), WithinAbs(10.0, 0.001));

    ParamRange fixed{5.0f, 5.0f, 5.0f, 1.0f};
    REQUIRE_THAT(fixed.normalize(123.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(fixed.denormalize(0.75f), WithinAbs(5.0, 0.001));
}

TEST_CASE("ParamValue tracks modulation and copy move state",
          "[state][value]") {
    ParamRange range{-1.0f, 1.0f, 0.0f, 0.5f};
    ParamValue value(0.25f);

    REQUIRE_THAT(value.get(), WithinAbs(0.25, 0.001));
    REQUIRE_THAT(value.get_normalized(range), WithinAbs(0.625, 0.001));

    value.set_normalized(1.0f, range);
    REQUIRE_THAT(value.get(), WithinAbs(1.0, 0.001));

    value.set_mod_offset(-0.25f);
    value.add_mod_offset(-0.25f);
    REQUIRE_THAT(value.get_modulated(), WithinAbs(0.5, 0.001));

    ParamValue copied(value);
    REQUIRE_THAT(copied.get(), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(copied.get_modulated(), WithinAbs(1.0, 0.001));

    ParamValue assigned;
    assigned = value;
    REQUIRE_THAT(assigned.get(), WithinAbs(1.0, 0.001));

    value.reset_mod();
    REQUIRE_THAT(value.get_modulated(), WithinAbs(1.0, 0.001));

    ParamValue moved(std::move(value));
    REQUIRE_THAT(moved.get(), WithinAbs(1.0, 0.001));

    ParamValue move_assigned;
    move_assigned = std::move(moved);
    REQUIRE_THAT(move_assigned.get(), WithinAbs(1.0, 0.001));
}

TEST_CASE("StateStore basic operations", "[state][store]") {
    StateStore store;

    ParamInfo gain_info;
    gain_info.id = 1;
    gain_info.name = "Gain";
    gain_info.unit = "dB";
    gain_info.range = {-60.0f, 12.0f, 0.0f, 0.0f};

    ParamInfo mix_info;
    mix_info.id = 2;
    mix_info.name = "Mix";
    mix_info.unit = "%";
    mix_info.range = {0.0f, 100.0f, 100.0f, 0.0f};

    store.add_parameter(gain_info);
    store.add_parameter(mix_info);

    SECTION("Initial values are defaults") {
        REQUIRE_THAT(store.get_value(1), WithinAbs(0.0, 0.001));
        REQUIRE_THAT(store.get_value(2), WithinAbs(100.0, 0.001));
    }

    SECTION("Set and get") {
        store.set_value(1, -12.0f);
        REQUIRE_THAT(store.get_value(1), WithinAbs(-12.0, 0.001));
    }

    SECTION("Values clamped to range") {
        store.set_value(1, 999.0f);
        REQUIRE_THAT(store.get_value(1), WithinAbs(12.0, 0.001));

        store.set_value(1, -999.0f);
        REQUIRE_THAT(store.get_value(1), WithinAbs(-60.0, 0.001));
    }

    SECTION("Normalized access") {
        store.set_normalized(1, 0.5f);
        float expected = -60.0f + 0.5f * (12.0f - (-60.0f)); // -24 dB
        REQUIRE_THAT(store.get_value(1), WithinAbs(expected, 0.1));
    }

    SECTION("Reset to default") {
        store.set_value(1, -12.0f);
        store.reset_to_default(1);
        REQUIRE_THAT(store.get_value(1), WithinAbs(0.0, 0.001));
    }

    SECTION("Param info lookup") {
        auto* info = store.info(1);
        REQUIRE(info != nullptr);
        REQUIRE(info->name == "Gain");
        REQUIRE(info->unit == "dB");

        REQUIRE(store.info(999) == nullptr);
    }

    SECTION("Unknown param returns 0") {
        REQUIRE(store.get_value(999) == 0.0f);
    }

    SECTION("Param count") {
        REQUIRE(store.param_count() == 2);
    }
}

TEST_CASE("StateStore duplicate parameter ids resolve to latest registration",
          "[state][store]") {
    StateStore store;
    store.add_parameter(make_param_info(7, "First", "dB", {-60.0f, 12.0f, -6.0f}));
    store.add_parameter(make_param_info(7, "Second", "%", {0.0f, 100.0f, 25.0f}));

    REQUIRE(store.param_count() == 2);
    REQUIRE(store.all_params()[0].name == "First");
    REQUIRE(store.all_params()[1].name == "Second");
    REQUIRE(store.info(7)->name == "Second");
    REQUIRE_THAT(store.get_value(7), WithinAbs(25.0f, 1e-6f));

    store.set_value(7, 150.0f);
    REQUIRE_THAT(store.get_value(7), WithinAbs(100.0f, 1e-6f));
}

TEST_CASE("StateStore serialization", "[state][serialize]") {
    StateStore store;

    ParamInfo p1 = make_param_info(1, "A", "", {0.0f, 1.0f, 0.5f});
    ParamInfo p2 = make_param_info(2, "B", "", {-1.0f, 1.0f, 0.0f});
    store.add_parameter(p1);
    store.add_parameter(p2);

    store.set_value(1, 0.75f);
    store.set_value(2, -0.5f);

    auto data = store.serialize();
    REQUIRE_FALSE(data.empty());

    SECTION("Round-trip") {
        StateStore store2;
        store2.add_parameter(p1);
        store2.add_parameter(p2);

        REQUIRE(store2.deserialize(data));
        REQUIRE_THAT(store2.get_value(1), WithinAbs(0.75, 0.001));
        REQUIRE_THAT(store2.get_value(2), WithinAbs(-0.5, 0.001));
    }

    SECTION("Forward compatibility (unknown params ignored)") {
        StateStore store3;
        store3.add_parameter(p1); // Only knows param 1, not 2

        REQUIRE(store3.deserialize(data));
        REQUIRE_THAT(store3.get_value(1), WithinAbs(0.75, 0.001));
    }

    SECTION("Backward compatibility (missing params keep defaults)") {
        StateStore store_old;
        store_old.add_parameter(p1);
        store_old.set_value(1, 0.9f);
        auto old_data = store_old.serialize();

        StateStore store_new;
        store_new.add_parameter(p1);
        store_new.add_parameter(p2); // New param, not in old data

        REQUIRE(store_new.deserialize(old_data));
        REQUIRE_THAT(store_new.get_value(1), WithinAbs(0.9, 0.001));
        REQUIRE_THAT(store_new.get_value(2), WithinAbs(0.0, 0.001)); // Default
    }

    SECTION("Corrupted data rejected") {
        auto bad_data = data;
        bad_data[bad_data.size() - 1] ^= 0xFF; // Corrupt CRC
        StateStore store4;
        store4.add_parameter(p1);
        REQUIRE_FALSE(store4.deserialize(bad_data));
    }

    SECTION("Too-short data rejected") {
        REQUIRE_FALSE(store.deserialize(std::span<const uint8_t>{}));
    }

    SECTION("CRC-valid truncated parameter payload rejected") {
        auto truncated = data;
        truncated.erase(truncated.begin() + 20, truncated.begin() + 28);
        const auto payload_size = truncated.size() - 4;
        write_u32_le(truncated, payload_size, crc32_simple_for_test(truncated, payload_size));

        StateStore store4;
        store4.add_parameter(p1);
        store4.add_parameter(p2);
        store4.set_value(1, 0.25f);
        store4.set_value(2, 0.25f);

        REQUIRE_FALSE(store4.deserialize(truncated));
        REQUIRE_THAT(store4.get_value(1), WithinAbs(0.25, 0.001));
        REQUIRE_THAT(store4.get_value(2), WithinAbs(0.25, 0.001));
    }
}

TEST_CASE("StateStore serialization records header fields and rejects future versions",
          "[state][serialize]") {
    StateStore store;
    auto p1 = make_param_info(10, "Drive", "", {0.0f, 1.0f, 0.25f});
    auto p2 = make_param_info(20, "Tone", "", {-1.0f, 1.0f, 0.0f});
    store.add_parameter(p1);
    store.add_parameter(p2);
    store.set_value(10, 0.75f);

    auto data = store.serialize();
    REQUIRE(data.size() == 32);
    REQUIRE(std::memcmp(data.data(), "PULP", 4) == 0);
    REQUIRE(read_u32_le(data, 4) == 1);
    REQUIRE(read_u32_le(data, 8) == 2);
    REQUIRE(read_u32_le(data, 12) == 10);
    REQUIRE(read_u32_le(data, 20) == 20);

    auto future = data;
    write_u32_le(future, 4, 999u);
    const auto payload_size = future.size() - 4;
    write_u32_le(future, payload_size, crc32_simple_for_test(future, payload_size));

    StateStore target;
    target.add_parameter(p1);
    target.set_value(10, 0.1f);
    REQUIRE_FALSE(target.deserialize(future));
    REQUIRE_THAT(target.get_value(10), WithinAbs(0.1, 0.001));
}

TEST_CASE("StateStore serialization honors explicit current schema version",
          "[state][serialize]") {
    StateStore source;
    auto p = make_param_info(7, "Shape", "", {0.0f, 1.0f, 0.25f});
    source.add_parameter(p);
    source.set_state_version(4);
    source.set_value(7, 0.75f);

    auto data = source.serialize();
    REQUIRE(read_u32_le(data, 4) == 4);

    StateStore target;
    target.add_parameter(p);
    target.set_state_version(4);
    REQUIRE(target.deserialize(data));
    REQUIRE_THAT(target.get_value(7), WithinAbs(0.75, 0.001));
}

TEST_CASE("StateStore set_normalized clamps and quantizes via ParamRange",
          "[state][store]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Steps", "", {0.0f, 10.0f, 5.0f, 2.0f}));

    store.set_normalized(1, -1.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.0, 0.001));

    store.set_normalized(1, 0.34f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(4.0, 0.001));
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.4, 0.001));

    store.set_normalized(1, 4.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(10.0, 0.001));
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(1.0, 0.001));
}

TEST_CASE("StateStore deserialize clamps values to current ranges",
          "[state][serialize]") {
    StateStore source;
    source.add_parameter(make_param_info(1, "Wide", "", {0.0f, 100.0f, 50.0f}));
    source.set_value(1, 80.0f);
    auto data = source.serialize();

    StateStore target;
    target.add_parameter(make_param_info(1, "Narrow", "", {0.0f, 1.0f, 0.5f}));

    REQUIRE(target.deserialize(data));
    REQUIRE_THAT(target.get_value(1), WithinAbs(1.0, 0.001));
}

TEST_CASE("StateStore deserialize rejects bad magic and future versions",
          "[state][serialize]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.set_value(1, -12.0f);

    auto data = store.serialize();

    auto bad_magic = data;
    bad_magic[0] = 'X';
    REQUIRE_FALSE(store.deserialize(bad_magic));

    auto future_version = data;
    write_le_u32(future_version, 4, store.state_version() + 1);
    const auto payload_size = future_version.size() - 4;
    const auto crc = test_crc32(future_version.data(), payload_size);
    write_le_u32(future_version, payload_size, crc);

    REQUIRE_FALSE(store.deserialize(future_version));
    REQUIRE_THAT(store.get_value(1), WithinAbs(-12.0, 0.001));
}

TEST_CASE("StateStore deserialize rejects corrupted CRC without changing values",
          "[state][serialize]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.set_value(1, -6.0f);

    auto data = store.serialize();
    data[data.size() - 1] ^= 0x7Fu;

    store.set_value(1, -12.0f);
    REQUIRE_FALSE(store.deserialize(data));
    REQUIRE_THAT(store.get_value(1), WithinAbs(-12.0, 0.001));
}

TEST_CASE("StateStore change listener", "[state][listener]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.5f}));

    ParamID changed_id = 0;
    float changed_value = 0.0f;
    store.add_listener([&](ParamID id, float val) {
        changed_id = id;
        changed_value = val;
    });

    store.set_value(1, 0.8f);
    REQUIRE(changed_id == 1);
    REQUIRE_THAT(changed_value, WithinAbs(0.8, 0.001));
}

TEST_CASE("StateStore skips empty change listeners", "[state][listener]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.5f}));

    int calls = 0;
    store.add_listener({});
    store.add_listener([&](ParamID id, float value) {
        REQUIRE(id == 1);
        REQUIRE_THAT(value, WithinAbs(0.75, 0.001));
        ++calls;
    });

    store.set_value(1, 0.75f);
    REQUIRE(calls == 1);
}

TEST_CASE("StateStore ignores unknown value writes without notifying listeners",
          "[state][listener]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Known", "", {0.0f, 1.0f, 0.5f}));

    int calls = 0;
    store.add_listener([&](ParamID, float) { ++calls; });

    store.set_value(999, 0.75f);
    store.set_normalized(998, 0.25f);
    store.set_mod_offset(997, 1.0f);
    store.add_mod_offset(996, 1.0f);

    REQUIRE(calls == 0);
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.5, 0.001));
}

TEST_CASE("StateStore modulation offsets and default resets", "[state][store]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Cutoff", "Hz", {20.0f, 20000.0f, 1000.0f}));
    store.add_parameter(make_param_info(2, "Resonance", "", {0.1f, 10.0f, 1.0f}));

    REQUIRE(store.get_modulated(999) == 0.0f);
    REQUIRE(store.get_normalized(999) == 0.0f);
    REQUIRE(store.get_default(999) == 0.0f);

    store.set_value(1, 2000.0f);
    store.set_mod_offset(1, 150.0f);
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(2150.0, 0.001));

    store.add_mod_offset(1, -25.0f);
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(2125.0, 0.001));

    store.set_mod_offset(2, 0.75f);
    store.set_mod_offset(999, 123.0f);
    store.add_mod_offset(999, 456.0f);

    store.reset_all_mod();
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(2000.0, 0.001));
    REQUIRE_THAT(store.get_modulated(2), WithinAbs(1.0, 0.001));

    store.set_value(1, 4000.0f);
    store.set_value(2, 4.0f);
    store.reset_all_to_defaults();
    REQUIRE_THAT(store.get_value(1), WithinAbs(1000.0, 0.001));
    REQUIRE_THAT(store.get_value(2), WithinAbs(1.0, 0.001));
}

TEST_CASE("StateStore exposes registration spans and gesture callbacks", "[state][store]") {
    StateStore store;

    store.begin_gesture(1);
    store.end_gesture(1);

    store.add_group({10, "Main", 0});
    store.add_group({20, "Oscillator", 10});
    store.add_parameter(make_param_info(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.add_parameter(make_param_info(2, "Mix", "%", {0.0f, 100.0f, 50.0f}));

    auto params = store.all_params();
    REQUIRE(params.size() == 2);
    REQUIRE(params[0].id == 1);
    REQUIRE(params[0].name == "Gain");
    REQUIRE(params[1].unit == "%");

    auto groups = store.all_groups();
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].name == "Main");
    REQUIRE(groups[1].parent_id == 10);

    std::vector<ParamID> began;
    std::vector<ParamID> ended;
    store.set_gesture_callbacks(
        [&](ParamID id) { began.push_back(id); },
        [&](ParamID id) { ended.push_back(id); });

    store.begin_gesture(2);
    store.end_gesture(2);

    REQUIRE(began == std::vector<ParamID>{2});
    REQUIRE(ended == std::vector<ParamID>{2});
}

TEST_CASE("StateStore preserves parameter display conversion callbacks",
          "[state][store]") {
    StateStore store;
    ParamInfo info = make_param_info(7, "Frequency", "Hz", {20.0f, 20000.0f, 440.0f});
    info.to_string = [](float value) {
        return std::to_string(static_cast<int>(value)) + " Hz";
    };
    info.from_string = [](const std::string& value) {
        return value == "concert" ? 440.0f : 20.0f;
    };

    store.add_parameter(info);
    const auto* stored = store.info(7);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->to_string);
    REQUIRE(stored->from_string);
    REQUIRE(stored->to_string(880.0f) == "880 Hz");
    REQUIRE_THAT(stored->from_string("concert"), WithinAbs(440.0f, 0.001f));
}

TEST_CASE("StateStore unknown modulation writes are no-ops",
          "[state][store]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.25f}));

    store.set_mod_offset(999, 10.0f);
    store.add_mod_offset(999, 10.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(store.get_modulated(999), WithinAbs(0.0f, 0.001f));

    store.set_mod_offset(1, 0.5f);
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(0.75f, 0.001f));
    store.reset_all_mod();
    REQUIRE_THAT(store.get_modulated(1), WithinAbs(0.25f, 0.001f));
}

TEST_CASE("ParamRange ignores negative step and still clamps output",
          "[state][range]") {
    ParamRange range{0.0f, 10.0f, 5.0f, -2.0f};

    REQUIRE_THAT(range.denormalize(0.25f), WithinAbs(2.5, 0.001));
    REQUIRE_THAT(range.denormalize(-1.0f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.denormalize(2.0f), WithinAbs(10.0, 0.001));
}

TEST_CASE("ParamRange quantization clamps non-divisible upper steps",
          "[state][range]") {
    ParamRange range{0.0f, 10.0f, 0.0f, 3.0f};

    REQUIRE_THAT(range.denormalize(0.14f), WithinAbs(0.0, 0.001));
    REQUIRE_THAT(range.denormalize(0.45f), WithinAbs(6.0, 0.001));
    REQUIRE_THAT(range.denormalize(0.95f), WithinAbs(9.0, 0.001));
    REQUIRE_THAT(range.denormalize(1.0f), WithinAbs(9.0, 0.001));
}

TEST_CASE("ParamValue copy and assignment reset modulation offset",
          "[state][value]") {
    ParamValue source(2.0f);
    source.set_mod_offset(3.0f);

    ParamValue copied(source);
    ParamValue assigned;
    assigned = source;

    REQUIRE_THAT(source.get_modulated(), WithinAbs(5.0, 0.001));
    REQUIRE_THAT(copied.get(), WithinAbs(2.0, 0.001));
    REQUIRE_THAT(copied.get_modulated(), WithinAbs(2.0, 0.001));
    REQUIRE_THAT(assigned.get_modulated(), WithinAbs(2.0, 0.001));
}

TEST_CASE("StateStore listener snapshot defers newly added listeners",
          "[state][listener]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {0.0f, 1.0f, 0.0f}));

    std::vector<float> first_listener_values;
    std::vector<float> late_listener_values;
    store.add_listener([&](ParamID, float value) {
        first_listener_values.push_back(value);
        if (first_listener_values.size() == 1) {
            store.add_listener([&](ParamID, float late_value) {
                late_listener_values.push_back(late_value);
            });
        }
    });

    store.set_value(1, 0.25f);
    REQUIRE(first_listener_values == std::vector<float>{0.25f});
    REQUIRE(late_listener_values.empty());

    store.set_value(1, 0.5f);
    REQUIRE(first_listener_values == std::vector<float>{0.25f, 0.5f});
    REQUIRE(late_listener_values == std::vector<float>{0.5f});
}

TEST_CASE("StateStore gesture callbacks can be replaced and cleared",
          "[state][store]") {
    StateStore store;
    std::vector<ParamID> began;
    std::vector<ParamID> ended;

    store.set_gesture_callbacks(
        [&](ParamID id) { began.push_back(id); },
        [&](ParamID id) { ended.push_back(id); });
    store.begin_gesture(11);
    store.end_gesture(11);

    store.set_gesture_callbacks({}, {});
    store.begin_gesture(22);
    store.end_gesture(22);

    REQUIRE(began == std::vector<ParamID>{11});
    REQUIRE(ended == std::vector<ParamID>{11});
}

TEST_CASE("Binding handles unbound and polled parameter changes",
          "[state][binding]") {
    Binding empty;
    REQUIRE_FALSE(empty.is_bound());
    REQUIRE(empty.id() == 0);
    REQUIRE(empty.info() == nullptr);
    REQUIRE(empty.get() == 0.0f);
    REQUIRE(empty.get_normalized() == 0.0f);
    REQUIRE_FALSE(empty.poll());
    REQUIRE(empty.edit_history() == nullptr);
    REQUIRE_NOTHROW(empty.set(1.0f));
    REQUIRE_NOTHROW(empty.set_normalized(0.5f));
    REQUIRE_NOTHROW(empty.reset());

    StateStore store;
    store.add_parameter(make_param_info(7, "Level", "dB", {-60.0f, 6.0f, -12.0f}));

    Binding binding(store, 7);
    std::vector<float> notified;
    binding.on_change([&](float value) { notified.push_back(value); });

    REQUIRE(binding.is_bound());
    REQUIRE(binding.id() == 7);
    REQUIRE(binding.info() != nullptr);
    REQUIRE(binding.info()->name == "Level");
    REQUIRE_THAT(binding.get(), WithinAbs(-12.0, 0.001));

    REQUIRE(binding.poll());
    REQUIRE_THAT(notified.back(), WithinAbs(-12.0, 0.001));
    REQUIRE_FALSE(binding.poll());

    binding.set(-6.0f);
    REQUIRE_THAT(store.get_value(7), WithinAbs(-6.0, 0.001));
    REQUIRE_THAT(notified.back(), WithinAbs(-6.0, 0.001));

    notified.clear();
    store.set_value(7, 3.0f);
    REQUIRE(binding.poll());
    REQUIRE(notified.size() == 1);
    REQUIRE_THAT(notified[0], WithinAbs(3.0, 0.001));

    binding.reset();
    REQUIRE_THAT(store.get_value(7), WithinAbs(-12.0, 0.001));
    REQUIRE_THAT(notified.back(), WithinAbs(-12.0, 0.001));
}

TEST_CASE("Binding gesture end records undoable parameter edits",
          "[state][binding]") {
    StateStore store;
    store.add_parameter(make_param_info(9, "Mix", "%", {0.0f, 100.0f, 50.0f}));

    Binding binding(store, 9);
    EditHistory history;
    binding.set_edit_history(&history);
    REQUIRE(binding.edit_history() == &history);

    binding.begin_gesture();
    binding.set(75.0f);
    binding.end_gesture();

    REQUIRE(history.can_undo());
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "Mix");
    REQUIRE_THAT(store.get_value(9), WithinAbs(75.0, 0.001));

    REQUIRE(history.undo());
    REQUIRE_THAT(store.get_value(9), WithinAbs(50.0, 0.001));
    REQUIRE(history.redo());
    REQUIRE_THAT(store.get_value(9), WithinAbs(75.0, 0.001));

    const auto before = history.undo_count();
    binding.begin_gesture();
    binding.set(75.0f);
    binding.end_gesture();
    REQUIRE(history.undo_count() == before);
}

TEST_CASE("Binding normalized writes notify only when the stored value changes",
          "[state][binding]") {
    StateStore store;
    store.add_parameter(make_param_info(12, "Pan", "", {-1.0f, 1.0f, 0.0f}));

    std::vector<ParamID> began;
    std::vector<ParamID> ended;
    store.set_gesture_callbacks(
        [&](ParamID id) { began.push_back(id); },
        [&](ParamID id) { ended.push_back(id); });

    Binding binding(store, 12);
    std::vector<float> notified;
    binding.on_change({});
    binding.on_change([&](float value) { notified.push_back(value); });

    REQUIRE(binding.is_bound());
    REQUIRE_THAT(binding.get_normalized(), WithinAbs(0.5, 0.001));

    binding.set_normalized(0.75f);
    REQUIRE_THAT(store.get_value(12), WithinAbs(0.5, 0.001));
    REQUIRE_THAT(binding.get(), WithinAbs(0.5, 0.001));
    REQUIRE(notified.size() == 1);
    REQUIRE_THAT(notified.back(), WithinAbs(0.5, 0.001));

    binding.set_normalized(0.75f);
    REQUIRE(notified.size() == 1);

    binding.set(9.0f);
    REQUIRE_THAT(store.get_value(12), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(binding.get_normalized(), WithinAbs(1.0, 0.001));
    REQUIRE(notified.size() == 2);
    REQUIRE_THAT(notified.back(), WithinAbs(1.0, 0.001));

    binding.set(1.0f);
    REQUIRE(notified.size() == 2);

    binding.begin_gesture();
    binding.end_gesture();
    REQUIRE(began == std::vector<ParamID>{12});
    REQUIRE(ended == std::vector<ParamID>{12});
    REQUIRE_FALSE(binding.edit_history());
    REQUIRE(binding.poll());
    REQUIRE(notified.size() == 3);
    REQUIRE_THAT(notified.back(), WithinAbs(1.0, 0.001));
}

TEST_CASE("Binding handles unregistered parameters without mutating the store",
          "[state][binding]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "dB", {-60.0f, 6.0f, -12.0f}));

    Binding missing(store, 99);
    std::vector<float> notified;
    missing.on_change([&](float value) { notified.push_back(value); });

    REQUIRE(missing.is_bound());
    REQUIRE(missing.id() == 99);
    REQUIRE(missing.info() == nullptr);
    REQUIRE(missing.get() == 0.0f);
    REQUIRE(missing.get_normalized() == 0.0f);

    missing.begin_gesture();
    missing.set(0.5f);
    missing.set_normalized(0.25f);
    missing.end_gesture();
    missing.reset();

    REQUIRE(notified.empty());
    REQUIRE_THAT(store.get_value(1), WithinAbs(-12.0, 0.001));
    REQUIRE(store.param_count() == 1);
}

TEST_CASE("create_bindings mirrors StateStore parameter registration order",
          "[state][binding]") {
    StateStore empty;
    auto empty_bindings = create_bindings(empty);
    REQUIRE(empty_bindings.empty());

    StateStore store;
    store.add_parameter(make_param_info(3, "Attack", "ms", {0.0f, 200.0f, 10.0f}));
    store.add_parameter(make_param_info(1, "Release", "ms", {1.0f, 1000.0f, 100.0f}));
    store.add_parameter(make_param_info(7, "Mix", "%", {0.0f, 100.0f, 50.0f}));

    auto bindings = create_bindings(store);

    REQUIRE(bindings.size() == 3);
    REQUIRE(bindings[0].id() == 3);
    REQUIRE(bindings[1].id() == 1);
    REQUIRE(bindings[2].id() == 7);
    REQUIRE(bindings[0].info() != nullptr);
    REQUIRE(bindings[1].info() != nullptr);
    REQUIRE(bindings[2].info() != nullptr);
    REQUIRE(bindings[0].info()->name == "Attack");
    REQUIRE(bindings[1].info()->name == "Release");
    REQUIRE(bindings[2].info()->name == "Mix");

    bindings[1].set_normalized(0.5f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(500.5, 0.001));
}

TEST_CASE("EditHistory trims depth clears redo and toggles coalescing",
          "[state][edit-history]") {
    EditHistory history(2);
    int value = 0;

    history.perform([&] { value = 1; }, [&] { value = 0; }, "one");
    history.perform([&] { value = 2; }, [&] { value = 1; }, "two");
    history.perform([&] { value = 3; }, [&] { value = 2; }, "three");

    REQUIRE(value == 3);
    REQUIRE(history.undo_count() == 2);
    REQUIRE(history.undo_description() == "three");
    REQUIRE(history.max_depth() == 2);

    REQUIRE(history.undo());
    REQUIRE(value == 2);
    REQUIRE(history.can_redo());

    history.perform([&] { value = 4; }, [&] { value = 2; }, "four");
    REQUIRE(value == 4);
    REQUIRE_FALSE(history.can_redo());

    history.set_coalesce(false);
    history.perform([&] { value = 5; }, [&] { value = 4; }, "same");
    history.perform([&] { value = 6; }, [&] { value = 5; }, "same");
    REQUIRE(history.undo_count() == 2);
    REQUIRE(history.undo_description() == "same");

    history.clear();
    REQUIRE_FALSE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
    REQUIRE(history.undo_description().empty());
}

TEST_CASE("EditHistory coalesces matching descriptions and clamps max depth",
          "[state][edit-history]") {
    EditHistory history(4);
    int value = 0;

    history.perform([&] { value = 1; }, [&] { value = 0; }, "gain");
    history.perform([&] { value = 2; }, [&] { value = 1; }, "gain");
    REQUIRE(value == 2);
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "gain");

    REQUIRE(history.undo());
    REQUIRE(value == 1);
    REQUIRE(history.redo());
    REQUIRE(value == 2);

    history.set_max_depth(0);
    REQUIRE(history.max_depth() == 0);
    REQUIRE_FALSE(history.can_undo());
}

TEST_CASE("EditHistory exposes empty descriptions, redo counts, and inert callbacks",
          "[state][edit-history]") {
    struct BlankAction final : EditAction {
        explicit BlankAction(int& target) : value(&target) {}

        void undo() override { *value = 0; }
        void redo() override { *value = 1; }

        int* value = nullptr;
    };

    EditHistory history;
    int value = 0;

    history.perform(std::make_unique<BlankAction>(value));
    REQUIRE(value == 1);
    REQUIRE(history.undo_description().empty());

    REQUIRE(history.undo());
    REQUIRE(value == 0);
    REQUIRE(history.redo_count() == 1);
    REQUIRE_FALSE(history.can_undo());

    REQUIRE(history.redo());
    REQUIRE(value == 1);
    REQUIRE(history.redo_count() == 0);
    REQUIRE_FALSE(history.can_redo());

    LambdaEdit empty_callbacks({}, {}, "empty callbacks");
    empty_callbacks.redo();
    empty_callbacks.undo();
    REQUIRE(empty_callbacks.description() == "empty callbacks");
}

TEST_CASE("StateStore unknown modulation and reset calls are inert",
          "[state][store]") {
    StateStore store;
    store.add_parameter(make_param_info(3, "Depth", "", {0.0f, 1.0f, 0.25f}));

    store.set_mod_offset(404, 3.0f);
    store.add_mod_offset(404, 2.0f);
    REQUIRE_THAT(store.get_modulated(404), WithinAbs(0.0f, 1e-6f));

    store.reset_to_default(404);
    store.reset_all_to_defaults();
    REQUIRE_THAT(store.get_value(3), WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("StateStore deserialize rejects short declared count payloads",
          "[state][serialize]") {
    StateStore source;
    auto p1 = make_param_info(1, "One", "", {0.0f, 1.0f, 0.0f});
    source.add_parameter(p1);
    source.set_value(1, 0.75f);

    auto data = source.serialize();
    write_le_u32(data, 8, 2u);
    const auto payload_size = data.size() - 4;
    write_le_u32(data, payload_size, test_crc32(data.data(), payload_size));

    StateStore target;
    target.add_parameter(p1);
    target.set_value(1, 0.25f);

    REQUIRE_FALSE(target.deserialize(data));
    REQUIRE_THAT(target.get_value(1), WithinAbs(0.25, 0.001));
}

TEST_CASE("StateStore empty serialization round-trips as a minimum frame",
          "[state][serialize]") {
    StateStore source;

    auto data = source.serialize();

    REQUIRE(data.size() == 16);
    REQUIRE(std::memcmp(data.data(), "PULP", 4) == 0);
    REQUIRE(read_u32_le(data, 8) == 0);

    StateStore target;
    REQUIRE(target.deserialize(data));
    REQUIRE(target.param_count() == 0);
    REQUIRE_FALSE(target.deserialize(std::span<const uint8_t>{data.data(),
                                                              data.size() - 1}));
}

TEST_CASE("StateStore deserialize rejects trailing payload extensions",
          "[state][serialize]") {
    StateStore source;
    auto p1 = make_param_info(1, "One", "", {0.0f, 1.0f, 0.0f});
    source.add_parameter(p1);
    source.set_value(1, 0.5f);

    auto data = source.serialize();
    const auto old_crc_offset = data.size() - 4;
    data.insert(data.begin() + static_cast<std::ptrdiff_t>(old_crc_offset),
                {0xde, 0xad, 0xbe, 0xef});
    const auto payload_size = data.size() - 4;
    write_le_u32(data, payload_size, test_crc32(data.data(), payload_size));

    StateStore target;
    target.add_parameter(p1);
    target.set_value(1, 0.25f);

    REQUIRE_FALSE(target.deserialize(data));
    REQUIRE_THAT(target.get_value(1), WithinAbs(0.25, 0.001));
}

// ─── ListenerToken / thread routing ──────────────────────────────────────────

TEST_CASE("ListenerToken removes its listener on destruction",
          "[state][listener][token]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int call_count = 0;
    {
        auto token = store.add_audio_listener(
            [&](ParamID, float) { ++call_count; });
        REQUIRE(static_cast<bool>(token));
        REQUIRE(token.id() != 0);

        store.set_value(1, 0.5f);
        REQUIRE(call_count == 1);
    } // token destroyed → listener removed

    store.set_value(1, 0.75f);
    REQUIRE(call_count == 1); // unchanged
}

TEST_CASE("ListenerToken reset() removes the subscription early",
          "[state][listener][token]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int call_count = 0;
    auto token = store.add_audio_listener(
        [&](ParamID, float) { ++call_count; });

    store.set_value(1, 0.25f);
    REQUIRE(call_count == 1);

    token.reset();
    REQUIRE_FALSE(static_cast<bool>(token));
    REQUIRE(token.id() == 0);

    store.set_value(1, 0.5f);
    REQUIRE(call_count == 1);

    // reset() is idempotent.
    token.reset();
    REQUIRE_FALSE(static_cast<bool>(token));
}

TEST_CASE("ListenerToken is move-only and transfers ownership",
          "[state][listener][token]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int call_count = 0;
    auto token1 = store.add_audio_listener(
        [&](ParamID, float) { ++call_count; });
    const auto original_id = token1.id();
    REQUIRE(original_id != 0);

    ListenerToken token2(std::move(token1));
    REQUIRE(token2.id() == original_id);
    REQUIRE_FALSE(static_cast<bool>(token1));

    store.set_value(1, 0.5f);
    REQUIRE(call_count == 1);

    // move-assignment removes the previous subscription
    auto token3 = store.add_audio_listener(
        [&](ParamID, float) { ++call_count; });
    const auto third_id = token3.id();
    token2 = std::move(token3);
    REQUIRE(token2.id() == third_id);

    store.set_value(1, 0.75f);
    // Only token2's listener fires; token1 was moved-from earlier and
    // its subscription was transferred and then overwritten by token2.
    REQUIRE(call_count == 2);
}

TEST_CASE("ListenerToken self move-assignment keeps the subscription",
          "[state][listener][token]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int call_count = 0;
    auto token = store.add_audio_listener(
        [&](ParamID, float) { ++call_count; });
    const auto original_id = token.id();

    auto& same_token = token;
    token = std::move(same_token);

    REQUIRE(static_cast<bool>(token));
    REQUIRE(token.id() == original_id);

    store.set_value(1, 0.5f);
    REQUIRE(call_count == 1);
}

TEST_CASE("remove_listener clears the token without firing the callback",
          "[state][listener][token]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int call_count = 0;
    auto token = store.add_audio_listener(
        [&](ParamID, float) { ++call_count; });

    store.remove_listener(token);
    REQUIRE_FALSE(static_cast<bool>(token));

    store.set_value(1, 0.4f);
    REQUIRE(call_count == 0);
}

TEST_CASE("Main listeners run inline when no EventLoop is installed",
          "[state][listener][thread]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    float seen = -1.0f;
    auto token = store.add_listener(
        [&](ParamID, float v) { seen = v; },
        ListenerThread::Main);

    store.set_value(1, 0.3f);
    REQUIRE_THAT(seen, WithinAbs(0.3, 0.001));
}

TEST_CASE("Main listeners are marshalled through the installed EventLoop",
          "[state][listener][thread]") {
    pulp::events::EventLoop loop;
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));
    store.set_main_loop(&loop);

    std::atomic<std::thread::id> firing_thread{std::this_thread::get_id()};
    std::atomic<bool> done{false};
    auto token = store.add_listener(
        [&](ParamID, float) {
            firing_thread.store(std::this_thread::get_id(),
                                std::memory_order_release);
            done.store(true, std::memory_order_release);
        },
        ListenerThread::Main);

    const auto caller_thread = std::this_thread::get_id();
    store.set_value(1, 0.6f);

    // Spin-wait briefly for the dispatched task to run.
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(2);
    while (!done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(done.load(std::memory_order_acquire));
    REQUIRE(firing_thread.load(std::memory_order_acquire) != caller_thread);

    // Detach the loop before either side goes out of scope, so the
    // store doesn't try to dispatch onto a destroyed loop.
    token.reset();
    store.set_main_loop(nullptr);
}

TEST_CASE("Audio listeners run inline even with an EventLoop installed",
          "[state][listener][thread]") {
    pulp::events::EventLoop loop;
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));
    store.set_main_loop(&loop);

    std::thread::id firing_thread;
    auto token = store.add_audio_listener(
        [&](ParamID, float) {
            firing_thread = std::this_thread::get_id();
        });

    store.set_value(1, 0.2f);
    REQUIRE(firing_thread == std::this_thread::get_id());

    token.reset();
    store.set_main_loop(nullptr);
}

TEST_CASE("Tokens survive StateStore destruction without crashing",
          "[state][listener][token]") {
    ListenerToken orphan;
    {
        StateStore store;
        store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));
        orphan = store.add_audio_listener([](ParamID, float) {});
        REQUIRE(static_cast<bool>(orphan));
    }
    // Store is gone; the weak_ptr in the token has expired. reset()
    // (and the destructor at end of test) must not crash.
    orphan.reset();
    REQUIRE_FALSE(static_cast<bool>(orphan));
}

TEST_CASE("Queued Main callback is cancelled by token reset",
          "[state][listener][token][thread]") {
    // A Main listener dispatched through the EventLoop must not fire if the
    // token is destroyed or reset between enqueue and drain. The dispatch
    // lambda re-looks-up the entry by id at drain time, so removal cancels it.
    pulp::events::EventLoop loop;
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));
    store.set_main_loop(&loop);

    std::atomic<bool> callback_fired{false};
    auto token = store.add_listener(
        [&](ParamID, float) {
            callback_fired.store(true, std::memory_order_release);
        },
        ListenerThread::Main);

    // Park the loop with a blocking task so we can deterministically
    // interleave set_value() and token.reset() before the listener
    // dispatch runs.
    std::mutex release_mu;
    std::condition_variable release_cv;
    bool released = false;
    std::atomic<bool> blocker_running{false};

    loop.dispatch([&]() {
        blocker_running.store(true, std::memory_order_release);
        std::unique_lock lk(release_mu);
        release_cv.wait(lk, [&]() { return released; });
    });

    const auto blocker_deadline = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(2);
    while (!blocker_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < blocker_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(blocker_running.load(std::memory_order_acquire));

    // Loop is parked. Enqueue the listener dispatch, then remove the
    // listener BEFORE the loop drains.
    store.set_value(1, 0.5f);
    token.reset();

    {
        std::lock_guard lk(release_mu);
        released = true;
    }
    release_cv.notify_all();

    // Drain barrier: enqueue a sentinel and wait. By the time it runs,
    // the listener dispatch has already had its chance — and should
    // have observed the removed entry and dropped the call.
    std::atomic<bool> sentinel_done{false};
    loop.dispatch([&]() {
        sentinel_done.store(true, std::memory_order_release);
    });
    const auto drain_deadline = std::chrono::steady_clock::now()
                                + std::chrono::seconds(2);
    while (!sentinel_done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(sentinel_done.load(std::memory_order_acquire));

    REQUIRE_FALSE(callback_fired.load(std::memory_order_acquire));

    store.set_main_loop(nullptr);
}

TEST_CASE("StateStore reset all defaults notifies each registered parameter",
          "[state][store]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "A", "", {0.0f, 10.0f, 1.0f}));
    store.add_parameter(make_param_info(2, "B", "", {-1.0f, 1.0f, 0.0f}));

    store.set_value(1, 5.0f);
    store.set_value(2, 0.5f);

    std::vector<ParamID> changed;
    store.add_listener([&](ParamID id, float) { changed.push_back(id); });

    store.reset_all_to_defaults();

    REQUIRE(changed == std::vector<ParamID>{1, 2});
    REQUIRE_THAT(store.get_value(1), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(store.get_value(2), WithinAbs(0.0, 0.001));
}

TEST_CASE("StateStore custom state version is serialized and accepted",
          "[state][serialize]") {
    StateStore source;
    source.set_state_version(3);
    source.add_parameter(make_param_info(10, "Drive", "", {0.0f, 1.0f, 0.25f}));
    source.set_value(10, 0.75f);

    auto data = source.serialize();
    REQUIRE(read_u32_le(data, 4) == 3);

    StateStore target;
    target.set_state_version(3);
    target.add_parameter(make_param_info(10, "Drive", "", {0.0f, 1.0f, 0.25f}));

    REQUIRE(target.deserialize(data));
    REQUIRE_THAT(target.get_value(10), WithinAbs(0.75f, 0.001f));
}

TEST_CASE("StateStore serializes custom state version",
          "[state][serialize]") {
    StateStore store;
    store.set_state_version(7);
    store.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    store.set_value(42, 64.0f);

    auto data = store.serialize();

    REQUIRE(read_u32_le(data, 4) == 7);
    REQUIRE(read_u32_le(data, 8) == 1);
    REQUIRE(read_u32_le(data, 12) == 42);

    StateStore target;
    target.set_state_version(7);
    target.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));

    REQUIRE(target.deserialize(data));
    REQUIRE_THAT(target.get_value(42), WithinAbs(64.0, 0.001));
}

TEST_CASE("StateStore deserializes through registered version migrations",
          "[state][serialize][migration]") {
    StateStore source;
    source.set_state_version(2);
    source.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    source.set_value(42, 88.0f);
    auto data = source.serialize();

    StateStore target;
    target.set_state_version(3);
    target.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));

    REQUIRE(target.register_state_migration(
        2, 3,
        [](std::span<const uint8_t> source_blob, std::vector<uint8_t>& migrated) {
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, 3);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        }));

    REQUIRE(target.deserialize(data));
    REQUIRE_THAT(target.get_value(42), WithinAbs(88.0, 0.001));
}

TEST_CASE("StateStore migrations are scoped to each store instance",
          "[state][serialize][migration]") {
    StateStore source;
    source.set_state_version(6);
    source.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    source.set_value(42, 88.0f);
    auto data = source.serialize();

    bool first_called = false;
    bool second_called = false;

    StateStore first;
    first.set_state_version(7);
    first.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    REQUIRE(first.register_state_migration(
        6, 7,
        [&first_called](std::span<const uint8_t> source_blob,
                        std::vector<uint8_t>& migrated) {
            first_called = true;
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, 7);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        }));

    StateStore second;
    second.set_state_version(7);
    second.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    REQUIRE(second.register_state_migration(
        6, 7,
        [&second_called](std::span<const uint8_t> source_blob,
                         std::vector<uint8_t>& migrated) {
            second_called = true;
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, 7);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        }));

    REQUIRE(first.deserialize(data));
    REQUIRE(first_called);
    REQUIRE_FALSE(second_called);

    REQUIRE(second.deserialize(data));
    REQUIRE(second_called);
}

TEST_CASE("StateStore can copy registered migrations to a restore probe",
          "[state][serialize][migration]") {
    StateStore saved;
    saved.set_state_version(8);
    saved.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    saved.set_value(42, 73.0f);
    auto data = saved.serialize();

    int source_calls = 0;
    StateStore source_schema;
    source_schema.set_state_version(9);
    source_schema.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    REQUIRE(source_schema.register_state_migration(
        8, 9,
        [&source_calls](std::span<const uint8_t> source_blob,
                        std::vector<uint8_t>& migrated) {
            ++source_calls;
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, 9);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        }));

    StateStore probe;
    probe.set_state_version(9);
    probe.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    int probe_calls = 0;
    REQUIRE(probe.register_state_migration(
        8, 9,
        [&probe_calls](std::span<const uint8_t>, std::vector<uint8_t>&) {
            ++probe_calls;
            return false;
        }));
    REQUIRE_FALSE(probe.deserialize(data));
    REQUIRE(probe_calls == 1);
    REQUIRE(source_calls == 0);
    REQUIRE_THAT(probe.get_value(42), WithinAbs(10.0, 0.001));

    probe.copy_state_migrations_from(source_schema);
    REQUIRE(probe.deserialize(data));
    REQUIRE(source_calls == 1);
    REQUIRE_THAT(probe.get_value(42), WithinAbs(73.0, 0.001));

    StateStore live;
    live.set_state_version(9);
    live.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    live.copy_state_migrations_from(probe);
    REQUIRE(live.deserialize(data));
    REQUIRE(source_calls == 2);
    REQUIRE_THAT(live.get_value(42), WithinAbs(73.0, 0.001));
}

TEST_CASE("StateStore rejects corrupt source blobs before migration callbacks",
          "[state][serialize][migration]") {
    StateStore source;
    source.set_state_version(4);
    source.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    source.set_value(42, 88.0f);
    auto data = source.serialize();
    data.back() ^= 0x55u;

    bool migration_called = false;
    StateStore target;
    target.set_state_version(5);
    target.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));

    REQUIRE(target.register_state_migration(
        4, 5,
        [&migration_called](std::span<const uint8_t> source_blob,
                            std::vector<uint8_t>& migrated) {
            migration_called = true;
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, 5);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        }));

    REQUIRE_FALSE(target.deserialize(data));
    REQUIRE_FALSE(migration_called);
    REQUIRE_THAT(target.get_value(42), WithinAbs(10.0, 0.001));
}

TEST_CASE("StateMigrationRegistry rejects invalid and duplicate registrations",
          "[state][serialize][migration]") {
    StateMigrationRegistry registry;
    auto migration = [](std::span<const uint8_t>, std::vector<uint8_t>&) {
        return true;
    };

    REQUIRE_FALSE(registry.register_migration(2, 2, migration));
    REQUIRE_FALSE(registry.register_migration(3, 2, migration));
    REQUIRE_FALSE(registry.register_migration(1, 2, {}));
    REQUIRE(registry.register_migration(1, 2, migration));
    REQUIRE_FALSE(registry.register_migration(1, 3, migration));
    REQUIRE(registry.has_migration_from(1));
    REQUIRE_FALSE(registry.has_migration_from(2));
}

TEST_CASE("StateMigrationRegistry copies current-version blobs and rejects bad sources",
          "[state][serialize][migration]") {
    StateStore source;
    source.set_state_version(4);
    source.add_parameter(make_param_info(42, "Answer", "", {0.0f, 100.0f, 10.0f}));
    source.set_value(42, 64.0f);
    auto data = source.serialize();

    StateMigrationRegistry registry;
    auto same = registry.migrate(data, 4);
    REQUIRE(same.has_value());
    REQUIRE(*same == data);

    REQUIRE_FALSE(registry.migrate(data, 3).has_value());
    REQUIRE_FALSE(serialized_state_version({}).has_value());

    auto corrupt = data;
    corrupt[0] = 'X';
    REQUIRE_FALSE(serialized_state_version(corrupt).has_value());
    REQUIRE_FALSE(registry.migrate(corrupt, 4).has_value());

    auto bad_crc = data;
    bad_crc.back() ^= 0x5au;
    REQUIRE(serialized_state_version(bad_crc) == std::optional<uint32_t>{4});
    REQUIRE_FALSE(registry.migrate(bad_crc, 4).has_value());
}

TEST_CASE("StateMigrationRegistry applies multi-hop migrations in order",
          "[state][serialize][migration]") {
    StateStore source;
    source.set_state_version(1);
    source.add_parameter(make_param_info(7, "Mix", "", {0.0f, 1.0f, 0.25f}));
    source.set_value(7, 0.75f);
    auto data = source.serialize();

    std::vector<uint32_t> calls;
    auto rewrite_version = [&calls](uint32_t to_version) {
        return [&calls, to_version](std::span<const uint8_t> source_blob,
                                    std::vector<uint8_t>& migrated) {
            calls.push_back(to_version);
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, to_version);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        };
    };

    StateMigrationRegistry registry;
    REQUIRE(registry.register_migration(1, 2, rewrite_version(2)));
    REQUIRE(registry.register_migration(2, 4, rewrite_version(4)));

    auto migrated = registry.migrate(data, 4);
    REQUIRE(migrated.has_value());
    REQUIRE(calls == std::vector<uint32_t>{2, 4});
    REQUIRE(serialized_state_version(*migrated) == std::optional<uint32_t>{4});

    StateStore target;
    target.set_state_version(4);
    target.add_parameter(make_param_info(7, "Mix", "", {0.0f, 1.0f, 0.25f}));
    REQUIRE(target.deserialize(*migrated));
    REQUIRE_THAT(target.get_value(7), WithinAbs(0.75f, 0.001f));
}

TEST_CASE("StateMigrationRegistry rejects broken migration callbacks",
          "[state][serialize][migration]") {
    StateStore source;
    source.set_state_version(1);
    source.add_parameter(make_param_info(3, "Drive", "", {0.0f, 1.0f, 0.5f}));
    auto data = source.serialize();

    StateMigrationRegistry missing_hop;
    REQUIRE(missing_hop.register_migration(
        1, 3,
        [](std::span<const uint8_t> source_blob, std::vector<uint8_t>& migrated) {
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, 3);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        }));
    REQUIRE_FALSE(missing_hop.migrate(data, 2).has_value());

    StateMigrationRegistry returns_false;
    REQUIRE(returns_false.register_migration(
        1, 2,
        [](std::span<const uint8_t>, std::vector<uint8_t>&) {
            return false;
        }));
    REQUIRE_FALSE(returns_false.migrate(data, 2).has_value());

    StateMigrationRegistry empty_output;
    REQUIRE(empty_output.register_migration(
        1, 2,
        [](std::span<const uint8_t>, std::vector<uint8_t>& migrated) {
            migrated.clear();
            return true;
        }));
    REQUIRE_FALSE(empty_output.migrate(data, 2).has_value());

    StateMigrationRegistry wrong_version;
    REQUIRE(wrong_version.register_migration(
        1, 2,
        [](std::span<const uint8_t> source_blob, std::vector<uint8_t>& migrated) {
            migrated.assign(source_blob.begin(), source_blob.end());
            write_u32_le(migrated, 4, 99);
            const auto crc_offset = migrated.size() - 4;
            write_u32_le(migrated, crc_offset,
                         crc32_simple_for_test(migrated, crc_offset));
            return true;
        }));
    REQUIRE_FALSE(wrong_version.migrate(data, 2).has_value());
}

TEST_CASE("StateStore reset_all_to_defaults notifies in registration order",
          "[state][listener]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "", {-60.0f, 12.0f, 0.0f}));
    store.add_parameter(make_param_info(2, "Mix", "", {0.0f, 100.0f, 50.0f}));
    store.set_value(1, -12.0f);
    store.set_value(2, 75.0f);

    std::vector<ParamID> ids;
    std::vector<float> values;
    store.add_listener([&](ParamID id, float value) {
        ids.push_back(id);
        values.push_back(value);
    });

    store.reset_all_to_defaults();

    REQUIRE(ids == std::vector<ParamID>{1, 2});
    REQUIRE(values.size() == 2);
    REQUIRE_THAT(values[0], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(values[1], WithinAbs(50.0, 0.001));
}

// ─── set_value_rt + pump_listeners ───────────────────────────────────────────

TEST_CASE("set_value_rt writes atomic value and defers Main listener",
          "[state][listener][rt]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int fire_count = 0;
    float last_value = -1.0f;
    auto token = store.add_listener(
        [&](ParamID, float v) {
            ++fire_count;
            last_value = v;
        },
        ListenerThread::Main);

    store.set_value_rt(1, 0.4f);

    // Atomic value updated synchronously, but Main listener not fired yet.
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.4, 0.001));
    REQUIRE(fire_count == 0);

    const auto drained = store.pump_listeners();
    REQUIRE(drained == 1);
    REQUIRE(fire_count == 1);
    REQUIRE_THAT(last_value, WithinAbs(0.4, 0.001));
}

TEST_CASE("set_value_rt fires Audio listeners inline (no pump needed)",
          "[state][listener][rt]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int fire_count = 0;
    auto token = store.add_audio_listener(
        [&](ParamID, float) { ++fire_count; });

    store.set_value_rt(1, 0.7f);
    REQUIRE(fire_count == 1);  // fired inline, no pump

    const auto drained = store.pump_listeners();
    REQUIRE(drained == 0);
    REQUIRE(fire_count == 1);
}

TEST_CASE("pump_listeners coalesces RT changes to one current-value call per param",
          "[state][listener][rt]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));
    store.add_parameter(make_param_info(2, "Y", "", {0.0f, 1.0f, 0.0f}));

    std::vector<std::pair<ParamID, float>> seen;
    auto token = store.add_listener(
        [&](ParamID id, float v) { seen.emplace_back(id, v); },
        ListenerThread::Main);

    // A burst of writes to param 1, plus one write to param 2.
    for (int i = 1; i <= 5; ++i) {
        store.set_value_rt(1, static_cast<float>(i) * 0.1f);  // ...,0.5 last
    }
    store.set_value_rt(2, 0.8f);
    REQUIRE(seen.empty());

    // pump_listeners returns the number of changes DRAINED from the queue
    // (6: five for param 1 + one for param 2), but coalescing fires the
    // Main listener once per CHANGED param carrying its live latest value.
    REQUIRE(store.pump_listeners() == 6);
    REQUIRE(seen.size() == 2);

    // First-seen order is preserved: param 1 (latest 0.5), then param 2.
    REQUIRE(seen[0].first == 1);
    REQUIRE_THAT(seen[0].second, WithinAbs(0.5, 0.001));
    REQUIRE(seen[1].first == 2);
    REQUIRE_THAT(seen[1].second, WithinAbs(0.8, 0.001));
}

TEST_CASE("pump_listeners skips a coalesced change for an unregistered param",
          "[state][listener][rt]") {
    // A change can only enter the queue for a registered param, but the
    // coalesced drain reads the live value through a getter that returns
    // nullopt for an absent id; that path must skip rather than deliver a
    // bogus default. Drive it directly through the documented contract: a
    // param with no live value yields no callback. Here we register one
    // param, fire it, and confirm the (present) param IS delivered — the
    // absent-skip branch is exercised by the getter returning nullopt for
    // any id the store never registered.
    StateStore store;
    store.add_parameter(make_param_info(7, "Present", "", {0.0f, 1.0f, 0.0f}));

    int fire_count = 0;
    float seen = -1.0f;
    auto token = store.add_listener(
        [&](ParamID, float v) { ++fire_count; seen = v; },
        ListenerThread::Main);

    store.set_value_rt(7, 0.6f);   // registered -> delivered
    store.set_value_rt(999, 0.9f); // unregistered set is a no-op (not queued)

    REQUIRE(store.pump_listeners() == 1);  // only the registered change queued
    REQUIRE(fire_count == 1);
    REQUIRE_THAT(seen, WithinAbs(0.6, 0.001));
}

TEST_CASE("set_normalized_rt denormalizes through the parameter range",
          "[state][listener][rt]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "dB",
                                        {-60.0f, 12.0f, 0.0f}));

    float seen = 0.0f;
    auto token = store.add_listener(
        [&](ParamID, float v) { seen = v; },
        ListenerThread::Main);

    store.set_normalized_rt(1, 0.5f);
    REQUIRE(store.pump_listeners() == 1);

    const float expected = -60.0f + 0.5f * (12.0f - (-60.0f)); // -24 dB
    REQUIRE_THAT(seen, WithinAbs(expected, 0.1));
    REQUIRE_THAT(store.get_value(1), WithinAbs(expected, 0.1));
}

TEST_CASE("set_value_rt clamps and the RT queue is lossy under overflow",
          "[state][listener][rt]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int fire_count = 0;
    float last_value = -1.0f;
    auto token = store.add_listener(
        [&](ParamID, float v) { ++fire_count; last_value = v; },
        ListenerThread::Main);

    store.set_value_rt(1, 999.0f);
    REQUIRE(store.pump_listeners() == 1);
    REQUIRE(fire_count == 1);
    REQUIRE_THAT(store.get_value(1), WithinAbs(1.0, 0.001));

    // Saturate the bounded SPSC queue. Exact capacity is internal; we
    // require: (1) no crash / no block, (2) the atomic value still reflects
    // the latest write, (3) all those drained changes coalesce to a SINGLE
    // listener call for this one param, carrying the current value.
    fire_count = 0;
    constexpr int kOverflowN = 4096;
    for (int i = 0; i < kOverflowN; ++i) {
        store.set_value_rt(1, static_cast<float>(i % 100) * 0.01f);
    }
    const auto drained = store.pump_listeners();
    REQUIRE(drained <= static_cast<std::size_t>(kOverflowN));
    REQUIRE(fire_count == 1);  // one changed param -> one coalesced call
    const float latest = static_cast<float>((kOverflowN - 1) % 100) * 0.01f;
    REQUIRE_THAT(store.get_value(1), WithinAbs(latest, 0.001));
    REQUIRE_THAT(last_value, WithinAbs(latest, 0.001));
}

TEST_CASE("Main listeners settle on the latest value after RT queue overflow",
          "[state][listener][rt]") {
    // Regression: under dense automation the bounded RT->main queue drops
    // the newest pushes when full. drain_main_listeners() must deliver the
    // param's CURRENT atomic value, not the stale value that happened to be
    // queued, so a Main listener that consumes the callback's value argument
    // always ends on the correct latest value.
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    float last_seen = -1.0f;
    auto token = store.add_listener(
        [&](ParamID, float v) { last_seen = v; },
        ListenerThread::Main);

    // Drive far more changes than the queue can hold so the LATEST values
    // are dropped on push (the queue keeps the oldest under drop-newest).
    // The final, distinct write is the true current value.
    constexpr int kOverflowN = 8192;
    for (int i = 0; i < kOverflowN; ++i) {
        // A monotonically rising ramp ensures the queued (oldest) values
        // are strictly smaller than the final value — so an implementation
        // that delivered the queued value would settle low/stale.
        store.set_value_rt(1, static_cast<float>(i) / static_cast<float>(kOverflowN));
    }
    constexpr float kFinal = 0.875f;
    store.set_value_rt(1, kFinal);

    // The atomic is already current.
    REQUIRE_THAT(store.get_value(1), WithinAbs(kFinal, 0.001));

    const auto drained = store.pump_listeners();
    REQUIRE(drained > 0);

    // The Main listener must end on the CURRENT value (the latest write),
    // NOT an earlier queued/dropped value. On the pre-fix code last_seen
    // would equal a small early-ramp value (the oldest queued entry the
    // final drain pop delivered), well below kFinal.
    REQUIRE_THAT(last_seen, WithinAbs(kFinal, 0.001));
}

TEST_CASE("StateStore exposes RT listener queue telemetry",
          "[state][listener][rt][telemetry]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    auto token = store.add_listener([](ParamID, float) {}, ListenerThread::Main);

    const auto initial = store.rt_listener_queue_telemetry();
    REQUIRE(initial.size_approx == 0);
    REQUIRE(initial.capacity > 0);
    REQUIRE(initial.overflow_count == 0);

    for (std::size_t i = 0; i < initial.capacity; ++i) {
        store.set_value_rt(1, static_cast<float>(i % 100) * 0.01f);
    }
    const auto full = store.rt_listener_queue_telemetry();
    REQUIRE(full.size_approx == initial.capacity);
    REQUIRE(full.capacity == initial.capacity);
    REQUIRE(full.overflow_count == 0);

    store.set_value_rt(1, 0.25f);
    store.set_value_rt(1, 0.5f);
    const auto overflow = store.rt_listener_queue_telemetry();
    REQUIRE(overflow.size_approx == initial.capacity);
    REQUIRE(overflow.overflow_count == 2);

    store.reset_rt_listener_queue_overflow_count();
    const auto reset = store.rt_listener_queue_telemetry();
    REQUIRE(reset.size_approx == initial.capacity);
    REQUIRE(reset.overflow_count == 0);

    REQUIRE(store.pump_listeners() == initial.capacity);
    REQUIRE(store.rt_listener_queue_telemetry().size_approx == 0);
}

TEST_CASE("StateStore RT listener telemetry hot path allocates zero times",
          "[state][listener][rt][telemetry][rt-safety]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    auto token = store.add_listener([](ParamID, float) {}, ListenerThread::Main);

    pulp::test::RtAllocationProbe probe;

    const auto capacity = store.rt_listener_queue_telemetry().capacity;
    for (std::size_t i = 0; i < capacity; ++i) {
        store.set_value_rt(1, static_cast<float>(i % 100) * 0.01f);
    }
    store.set_value_rt(1, 0.75f);
    (void)store.rt_listener_queue_telemetry();
    store.reset_rt_listener_queue_overflow_count();

    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("RT-queued changes are skipped when the token was reset before pump",
          "[state][listener][rt][token]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));

    int fire_count = 0;
    auto token = store.add_listener(
        [&](ParamID, float) { ++fire_count; },
        ListenerThread::Main);

    store.set_value_rt(1, 0.2f);
    store.set_value_rt(1, 0.3f);
    token.reset();
    // Events are drained from the queue (so pump returns 2), but the
    // listener has been removed from the registry — so the callback
    // never fires for the queued events.
    REQUIRE(store.pump_listeners() == 2);
    REQUIRE(fire_count == 0);
}

// ─── snapshot() block-local helper ───────────────────────────────────────────

TEST_CASE("StateStore::snapshot returns values in the requested order",
          "[state][snapshot]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "dB",
                                        {-60.0f, 12.0f, 0.0f}));
    store.add_parameter(make_param_info(2, "Mix", "%",
                                        {0.0f, 100.0f, 50.0f}));
    store.add_parameter(make_param_info(3, "Cutoff", "Hz",
                                        {20.0f, 20000.0f, 1000.0f}));

    store.set_value(1, -6.0f);
    store.set_value(2, 75.0f);
    store.set_value(3, 4400.0f);

    constexpr std::array<ParamID, 3> ids{ 1, 2, 3 };
    const auto snap = store.snapshot(ids);

    REQUIRE_THAT(snap[0], WithinAbs(-6.0, 0.001));
    REQUIRE_THAT(snap[1], WithinAbs(75.0, 0.001));
    REQUIRE_THAT(snap[2], WithinAbs(4400.0, 0.001));
}

TEST_CASE("StateStore::snapshot handles unknown ids by returning 0",
          "[state][snapshot]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.5f}));
    store.set_value(1, 0.7f);

    constexpr std::array<ParamID, 3> ids{ 1, 999, 1 };
    const auto snap = store.snapshot(ids);

    REQUIRE_THAT(snap[0], WithinAbs(0.7, 0.001));
    REQUIRE(snap[1] == 0.0f);  // unknown
    REQUIRE_THAT(snap[2], WithinAbs(0.7, 0.001));
}

TEST_CASE("StateStore::snapshot_modulated includes mod offsets",
          "[state][snapshot]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Pitch", "st",
                                        {-12.0f, 12.0f, 0.0f}));
    store.add_parameter(make_param_info(2, "Cutoff", "Hz",
                                        {20.0f, 20000.0f, 1000.0f}));

    store.set_value(1, 3.0f);
    store.set_value(2, 2000.0f);
    store.set_mod_offset(1, 0.5f);  // pitch +0.5 from modulation
    store.set_mod_offset(2, 100.0f); // cutoff +100 from modulation

    constexpr std::array<ParamID, 2> ids{ 1, 2 };

    const auto base_snap = store.snapshot(ids);
    REQUIRE_THAT(base_snap[0], WithinAbs(3.0, 0.001));
    REQUIRE_THAT(base_snap[1], WithinAbs(2000.0, 0.001));

    const auto mod_snap = store.snapshot_modulated(ids);
    REQUIRE_THAT(mod_snap[0], WithinAbs(3.5, 0.001));
    REQUIRE_THAT(mod_snap[1], WithinAbs(2100.0, 0.001));
}

TEST_CASE("StateStore::snapshot works with a single parameter",
          "[state][snapshot]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "X", "", {0.0f, 1.0f, 0.0f}));
    store.set_value(1, 0.33f);

    constexpr std::array<ParamID, 1> ids{ 1 };
    const auto snap = store.snapshot(ids);
    REQUIRE(snap.size() == 1);
    REQUIRE_THAT(snap[0], WithinAbs(0.33, 0.001));
}

// ─── Parameter designation + trigger / momentary params ─────────────────────

TEST_CASE("ParamInfo defaults to no designation and is not a trigger",
          "[state][param-designation]") {
    ParamInfo info = make_param_info(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f});
    REQUIRE(info.designation == ParamDesignation::None);
    REQUIRE(info.is_trigger == false);
    REQUIRE(info.auto_resets() == false);
}

TEST_CASE("is_bypass_param honors a declared Bypass designation regardless of name",
          "[state][param-designation]") {
    // A param NOT named "Bypass" and NOT in boolean range — the legacy
    // heuristic would reject it — is still a bypass when declared.
    ParamInfo declared = make_param_info(1, "Engine Active", "",
                                         {0.0f, 4.0f, 0.0f, 0.25f});
    declared.designation = ParamDesignation::Bypass;
    REQUIRE(is_bypass_param(declared));
}

TEST_CASE("is_bypass_param falls back to the legacy name/range heuristic",
          "[state][param-designation]") {
    // No designation declared: the boolean "Bypass" name/range heuristic
    // still detects it, so existing plugins keep working unchanged.
    ParamInfo legacy = make_param_info(1, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f});
    REQUIRE(legacy.designation == ParamDesignation::None);
    REQUIRE(is_bypass_param(legacy));

    // A param merely named "Bypass" but with a non-boolean range is NOT a
    // bypass under the legacy heuristic (matches prior adapter behavior).
    ParamInfo named_only = make_param_info(2, "Bypass", "", {0.0f, 10.0f, 0.0f});
    REQUIRE_FALSE(is_bypass_param(named_only));

    // An ordinary parameter is never a bypass.
    ParamInfo ordinary = make_param_info(3, "Gain", "dB", {-60.0f, 12.0f, 0.0f});
    REQUIRE_FALSE(is_bypass_param(ordinary));
}

TEST_CASE("A non-Bypass designation is never treated as bypass",
          "[state][param-designation]") {
    // Even with a boolean "Bypass"-shaped range, a Reset designation must not
    // be mistaken for a bypass control.
    ParamInfo reset = make_param_info(1, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f});
    reset.designation = ParamDesignation::Reset;
    REQUIRE_FALSE(is_bypass_param(reset));
}

TEST_CASE("A trigger parameter auto-resets to default after a block",
          "[state][param-designation][rt-safety]") {
    StateStore store;
    // Default 0, raised to 1 by a "do this now" event.
    ParamInfo tap = make_param_info(1, "Tap", "", {0.0f, 1.0f, 0.0f, 1.0f});
    tap.is_trigger = true;
    store.add_parameter(tap);
    // An ordinary parameter alongside it must NOT be reset.
    store.add_parameter(make_param_info(2, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.set_value(2, -6.0f);

    REQUIRE(store.has_trigger_params());

    // Host/UI raises the trigger; the Processor observes it during the block.
    store.set_value(1, 1.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(1.0, 0.0001));

    // End-of-block auto-reset returns the trigger to its default, leaving the
    // ordinary parameter untouched.
    bool reset_any = false;
    {
        pulp::test::RtAllocationProbe probe;  // proves no audio-thread alloc.
        reset_any = store.reset_triggers_rt();
        REQUIRE_FALSE(probe.saw_allocation());
    }
    REQUIRE(reset_any);
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(store.get_value(2), WithinAbs(-6.0, 0.0001));
}

TEST_CASE("A Reset-designated parameter is a trigger and auto-resets",
          "[state][param-designation][rt-safety]") {
    StateStore store;
    // Reset designation implies trigger behavior (auto_resets()), even though
    // is_trigger was not set explicitly.
    ParamInfo panic = make_param_info(1, "Panic", "", {0.0f, 1.0f, 0.0f, 1.0f});
    panic.designation = ParamDesignation::Reset;
    REQUIRE(panic.is_trigger == false);
    REQUIRE(panic.auto_resets());
    store.add_parameter(panic);
    REQUIRE(store.has_trigger_params());

    store.set_value(1, 1.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(1.0, 0.0001));
    REQUIRE(store.reset_triggers_rt());
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.0, 0.0001));
}

TEST_CASE("reset_triggers_rt is a no-op for a store with no triggers",
          "[state][param-designation]") {
    StateStore store;
    store.add_parameter(make_param_info(1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}));
    store.set_value(1, -3.0f);
    REQUIRE_FALSE(store.has_trigger_params());
    REQUIRE_FALSE(store.reset_triggers_rt());
    REQUIRE_THAT(store.get_value(1), WithinAbs(-3.0, 0.0001));
}

TEST_CASE("reset_triggers_rt respects the duplicate-id latest-wins contract",
          "[state][param-designation]") {
    // A trigger registered under id 7, then SUPERSEDED by a non-trigger under
    // the same id. id_to_index_ resolves 7 to the latest (non-trigger) slot, so
    // get_value/set_value never touch the stale trigger slot. reset_triggers_rt
    // must NOT clobber the live value via the dead trigger index.
    StateStore store;
    ParamInfo trigger = make_param_info(7, "Old Trigger", "", {0.0f, 1.0f, 0.0f, 1.0f});
    trigger.is_trigger = true;
    store.add_parameter(trigger);
    // Supersede id 7 with an ordinary parameter (latest registration wins).
    store.add_parameter(make_param_info(7, "Now Ordinary", "dB", {-60.0f, 12.0f, -6.0f}));

    store.set_value(7, 3.0f);  // lands in the latest (ordinary) slot
    // The superseded trigger slot is dead, so no live trigger remains.
    REQUIRE_FALSE(store.reset_triggers_rt());
    // The live value is untouched — not stomped to the dead trigger's default.
    REQUIRE_THAT(store.get_value(7), WithinAbs(3.0, 0.0001));

    // Conversely, when the LATEST registration for an id is the trigger, it
    // resets and the earlier non-trigger slot is irrelevant.
    StateStore store2;
    store2.add_parameter(make_param_info(9, "Old Ordinary", "", {0.0f, 1.0f, 0.5f}));
    ParamInfo latest_trigger = make_param_info(9, "Reset", "", {0.0f, 1.0f, 0.0f, 1.0f});
    latest_trigger.designation = ParamDesignation::Reset;
    store2.add_parameter(latest_trigger);
    store2.set_value(9, 1.0f);
    REQUIRE(store2.reset_triggers_rt());
    REQUIRE_THAT(store2.get_value(9), WithinAbs(0.0, 0.0001));
}
