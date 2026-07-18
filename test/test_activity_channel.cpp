#include <pulp/runtime/activity_channel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

using pulp::runtime::ActivityChannel;

TEST_CASE("ActivityChannel reports independent realtime activity lanes",
          "[runtime][activity]") {
    ActivityChannel<3> channel;
    ActivityChannel<3>::Sequence lane0 = 0;
    ActivityChannel<3>::Sequence lane1 = 0;

    REQUIRE(ActivityChannel<3>::lane_count() == 3);
    REQUIRE_FALSE(channel.consume(0, lane0));
    REQUIRE_FALSE(channel.consume(1, lane1));

    channel.signal(1);

    REQUIRE_FALSE(channel.consume(0, lane0));
    REQUIRE(channel.consume(1, lane1));
    REQUIRE(lane1 == 1);
    REQUIRE_FALSE(channel.consume(1, lane1));
}

TEST_CASE("ActivityChannel coalesces bursts into the newest sequence",
          "[runtime][activity]") {
    ActivityChannel<1> channel;
    ActivityChannel<1>::Sequence cursor = 0;

    channel.signal(0);
    channel.signal(0);
    channel.signal(0);

    REQUIRE(channel.consume(0, cursor));
    REQUIRE(cursor == 3);
    REQUIRE_FALSE(channel.consume(0, cursor));
}

TEST_CASE("ActivityChannel invalid lanes fail closed",
          "[runtime][activity]") {
    ActivityChannel<2> channel;
    ActivityChannel<2>::Sequence cursor = 7;

    channel.signal(2);

    REQUIRE(channel.sequence(2) == 0);
    REQUIRE_FALSE(channel.consume(2, cursor));
    REQUIRE(cursor == 7);
    REQUIRE(channel.sequence(0) == 0);
    REQUIRE(channel.sequence(1) == 0);
}

TEST_CASE("SharedActivityChannel outlives its producer owner",
          "[runtime][activity][lifecycle]") {
    auto producer = pulp::runtime::make_activity_channel<2>();
    std::weak_ptr<ActivityChannel<2>> lifetime = producer;
    auto retained_editor_handle = producer;

    producer->signal(1);
    producer.reset();

    REQUIRE_FALSE(lifetime.expired());
    ActivityChannel<2>::Sequence cursor = 0;
    REQUIRE(retained_editor_handle->consume(1, cursor));
    REQUIRE(cursor == 1);

    retained_editor_handle.reset();
    REQUIRE(lifetime.expired());
}

TEST_CASE("ActivityChannel remains race-free under concurrent signal and consume",
          "[runtime][activity][concurrent][race]") {
    constexpr std::uint32_t kSignals = 100'000;
    ActivityChannel<1> channel;
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (std::uint32_t i = 0; i < kSignals; ++i) channel.signal(0);
        producer_done.store(true, std::memory_order_release);
    });

    ActivityChannel<1>::Sequence cursor = 0;
    std::uint32_t observations = 0;
    while (!producer_done.load(std::memory_order_acquire) ||
           channel.sequence(0) != cursor) {
        if (channel.consume(0, cursor)) ++observations;
    }
    producer.join();

    REQUIRE(cursor == kSignals);
    REQUIRE(observations > 0);
}
