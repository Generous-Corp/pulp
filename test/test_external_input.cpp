#include "harness/rt_allocation_probe.hpp"
#include "support/external_input_replay.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <filesystem>
#include <pulp/runtime/external_input.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <type_traits>

using namespace pulp::runtime;
static_assert(std::is_trivially_copyable_v<ExternalInputFrame<1>>);
static_assert(std::is_trivially_copyable_v<ExternalInputFrame<64>>);

template <std::size_t N = 3>
static ExternalInputFrame<N> frame(std::uint32_t epoch = 1, std::uint64_t sequence = 1) {
    ExternalInputFrame<N> f;
    f.source_id = 7;
    f.epoch = epoch;
    f.sequence = sequence;
    f.provider_time_ns = sequence * 10;
    f.status = ExternalInputStatus::valid;
    f.confidence = 0.9f;
    f.count = static_cast<std::uint8_t>(N);
    for (std::size_t i = 0; i < N; ++i)
        f.values[i] = static_cast<float>(i + sequence);
    return f;
}

TEST_CASE("external input accepts and exposes a complete frame", "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.5f, 10, 3};
    REQUIRE(gate.submit(frame(), p) == ExternalInputVerdict::accepted);
    REQUIRE(gate.valid());
    REQUIRE(gate.values().size() == 3);
    REQUIRE(gate.value(1, -1) == 2);
    REQUIRE(gate.counters().accepted == 1);
    REQUIRE(gate.counters().epochs == 1);
}

TEST_CASE("external input verdict ordering and lifecycle", "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.5f, 5, 3};
    auto f = frame();
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::accepted);
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::unchanged);
    auto bad_source = f;
    bad_source.source_id = 8;
    bad_source.sequence = 2;
    REQUIRE(gate.submit(bad_source, p) == ExternalInputVerdict::rejected_source);
    auto denied = f;
    denied.sequence = 2;
    denied.status = ExternalInputStatus::denied;
    REQUIRE(gate.submit(denied, p) == ExternalInputVerdict::rejected_status);
    REQUIRE_FALSE(gate.valid());
    REQUIRE(gate.last_status() == ExternalInputStatus::denied);
    auto malformed = frame(1, 3);
    malformed.count = 0;
    REQUIRE(gate.submit(malformed, p) == ExternalInputVerdict::rejected_malformed);
    auto low = frame(1, 4);
    low.confidence = 0.1f;
    REQUIRE(gate.submit(low, p) == ExternalInputVerdict::rejected_confidence);
    auto accepted = frame(1, 5);
    REQUIRE(gate.submit(accepted, p) == ExternalInputVerdict::accepted);
    auto old = frame(1, 4);
    REQUIRE(gate.submit(old, p) == ExternalInputVerdict::rejected_order);
    REQUIRE_FALSE(gate.advance(4, p));
    REQUIRE(gate.valid());
    REQUIRE(gate.advance(1, p));
    REQUIRE_FALSE(gate.valid());
    REQUIRE(gate.counters().expired == 1);
    auto restart = frame(2, 1);
    REQUIRE(gate.submit(restart, p) == ExternalInputVerdict::accepted);
    REQUIRE(gate.epoch() == 2);
}

TEST_CASE("external input rejects non-finite and wrong shape", "[runtime][external-input]") {
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0.0f, 0, 3};
    auto f = frame();
    f.values[1] = std::numeric_limits<float>::quiet_NaN();
    f.sequence = 2;
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::rejected_malformed);
    f = frame(1, 3);
    f.count = 2;
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::rejected_malformed);
    f = frame(1, 4);
    f.confidence = 2.0f;
    REQUIRE(gate.submit(f, p) == ExternalInputVerdict::rejected_confidence);
}

TEST_CASE("external input gate and triple buffer are allocation free",
          "[runtime][external-input]") {
    TripleBuffer<ExternalInputFrame<3>> buffer;
    ExternalInputGate<3> gate;
    ExternalInputPolicy p{7, 0, 0, 3};
    pulp::test::RtAllocationProbe probe;
    for (std::uint64_t i = 1; i <= 10000; ++i) {
        auto f = frame(1, i);
        buffer.write(f);
        REQUIRE(gate.submit(buffer.read(), p) ==
                (i == 1 ? ExternalInputVerdict::accepted : ExternalInputVerdict::accepted));
    }
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("external input replay receipts classify positive, negative, and inconclusive fixtures",
          "[runtime][external-input]") {
    for (const auto* name : {"imu-pause-resume.jsonl", "negative-control.jsonl", "zero-expect.jsonl"}) {
        ExternalInputGate<64> gate;
        ExternalInputPolicy policy{1, 0.6f, 12000, 0};
        const auto receipt = pulp::test::run_external_input_replay(
            std::filesystem::path("test/fixtures/external-input") / name, gate, policy);
        if (std::string(name) == "imu-pause-resume.jsonl") REQUIRE(receipt.result == "pass");
        if (std::string(name) == "negative-control.jsonl") REQUIRE(receipt.result == "fail");
        if (std::string(name) == "zero-expect.jsonl") REQUIRE(receipt.result == "inconclusive");
    }
}

TEST_CASE("TouchDesigner sharing recipe fixture is replayed through the external input gate",
          "[runtime][external-input][touchdesigner]") {
    ExternalInputGate<1> gate;
    ExternalInputPolicy policy{7, 0.6f, 12000, 1};
    const auto receipt = pulp::test::run_external_input_replay(
        "test/fixtures/external-input/t7-touchdesigner-synthesized.jsonl", gate, policy);
    REQUIRE(receipt.result == "pass");
    REQUIRE(receipt.frames == 4);
    REQUIRE(receipt.advances == 1);
    REQUIRE(receipt.expects == 5);
    REQUIRE(receipt.failed == 0);
}
