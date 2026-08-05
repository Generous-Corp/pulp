#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/view/value_channel_set.hpp>

using namespace pulp::view;

namespace {
MeterFrame stereo(float rms, float peak) {
    MeterFrame frame;
    frame.channels = 2;
    frame.rms[0] = frame.rms[1] = rms;
    frame.peak[0] = frame.peak[1] = peak;
    return frame;
}
}  // namespace

TEST_CASE("value-channel telemetry has one exclusive reader attachment",
          "[view][value-channel][telemetry][lifecycle]") {
    ValueChannelSet channels;
    channels.declare_scalar("gain_reduction", "dB");
    channels.declare_meter("output");
    channels.declare_vector("spectrum", "Hz");
    channels.declare_events("onsets");

    auto attachment = channels.attach_telemetry();
    REQUIRE(attachment.valid());
    REQUIRE(attachment.channels().size() == 4);
    CHECK(attachment.channels()[0].info.name == "gain_reduction");
    CHECK(attachment.channels()[0].info.shape == ValueChannelShape::scalar);
    CHECK_FALSE(attachment.channels()[0].has_source_timestamp);
    CHECK(attachment.channels()[0].has_ui_snapshot_timestamp);
    CHECK_FALSE(attachment.channels()[3].has_ui_snapshot_timestamp);
    CHECK_FALSE(channels.attach_telemetry().valid());

    attachment = {};
    CHECK(channels.attach_telemetry().valid());
    ValueChannelSet empty;
    CHECK_FALSE(empty.attach_telemetry().valid());
}

TEST_CASE("continuous telemetry only snapshots values consumed by the UI reader",
          "[view][value-channel][telemetry]") {
    ValueChannelSet channels;
    auto* scalar = channels.declare_scalar("gain_reduction", "dB");
    auto attachment = channels.attach_telemetry();
    REQUIRE(scalar != nullptr);

    scalar->publish(-3.0f);
    ValueChannelTelemetrySnapshot snapshot;
    REQUIRE(attachment.read_continuous(0, snapshot));
    CHECK_FALSE(snapshot.available);

    CHECK(scalar->read() == Catch::Approx(-3.0f));
    REQUIRE(attachment.read_continuous(0, snapshot));
    REQUIRE(snapshot.available);
    CHECK(std::get<float>(snapshot.payload) == Catch::Approx(-3.0f));
    CHECK(snapshot.source_publication == 1);
    CHECK(snapshot.ui_snapshot_sequence == 1);
    CHECK(snapshot.ui_snapshot_time_ns > 0);

    scalar->publish(-6.0f);
    scalar->publish(-9.0f);
    REQUIRE(attachment.read_continuous(0, snapshot));
    CHECK(snapshot.source_publication == 1);
    scalar->read();
    REQUIRE(attachment.read_continuous(0, snapshot));
    CHECK(snapshot.source_publication == 3);
    CHECK(snapshot.ui_snapshot_sequence == 2);
    CHECK(std::get<float>(snapshot.payload) == Catch::Approx(-9.0f));
}

TEST_CASE("continuous telemetry preserves meter and oversized-vector payloads",
          "[view][value-channel][telemetry]") {
    ValueChannelSet channels;
    auto* meter = channels.declare_meter("output");
    auto* vector = channels.declare_vector("spectrum");
    auto attachment = channels.attach_telemetry();

    meter->publish(stereo(0.25f, 0.75f));
    meter->read();
    std::vector<float> oversized(VectorFrame::kMaxSamples + 31, 0.5f);
    oversized[0] = 11.0f;
    oversized[VectorFrame::kMaxSamples - 1] = 22.0f;
    vector->publish(oversized.data(), static_cast<int>(oversized.size()));
    vector->read();

    ValueChannelTelemetrySnapshot snapshot;
    REQUIRE(attachment.read_continuous(0, snapshot));
    const auto& meter_payload = std::get<MeterFrame>(snapshot.payload);
    CHECK(meter_payload.rms[0] == Catch::Approx(0.25f));
    CHECK(meter_payload.peak[1] == Catch::Approx(0.75f));

    REQUIRE(attachment.read_continuous(1, snapshot));
    const auto& vector_payload = std::get<VectorFrame>(snapshot.payload);
    REQUIRE(vector_payload.count == VectorFrame::kMaxSamples);
    CHECK(vector_payload.samples[0] == Catch::Approx(11.0f));
    CHECK(vector_payload.samples[VectorFrame::kMaxSamples - 1] ==
          Catch::Approx(22.0f));
}

TEST_CASE("event telemetry retains batches that arrive between UI frames",
          "[view][value-channel][telemetry][rt-safety]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    REQUIRE(events != nullptr);
    const ValueEvent before_attach{.frame_index = 4, .value = 0.0f};
    events->publish(&before_attach, 1);
    auto attachment = channels.attach_telemetry();

    ValueChannelTelemetryEventBatch batch;
    CHECK_FALSE(attachment.try_pop_events(0, batch));
    for (int value = 1; value <= 3; ++value) {
        const ValueEvent event{.frame_index = static_cast<std::uint32_t>(value),
                               .value = static_cast<float>(value)};
        events->publish(&event, 1);
    }
    for (std::uint64_t publication = 2; publication <= 4; ++publication) {
        REQUIRE(attachment.try_pop_events(0, batch));
        CHECK(batch.source_publication == publication);
        CHECK(batch.frame.events[0].value ==
              Catch::Approx(static_cast<float>(publication - 1)));
    }
    CHECK_FALSE(attachment.try_pop_events(0, batch));
    CHECK(events->read().events[0].value == Catch::Approx(3.0f));
}

TEST_CASE("event telemetry truncates batches and counts queue overflow",
          "[view][value-channel][telemetry][rt-safety]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    auto attachment = channels.attach_telemetry();

    std::vector<ValueEvent> oversized(EventFrame::kMaxEvents + 17);
    for (std::size_t i = 0; i < oversized.size(); ++i) {
        oversized[i] = {.frame_index = static_cast<std::uint32_t>(i),
                        .value = static_cast<float>(i)};
    }
    events->publish(oversized.data(), static_cast<int>(oversized.size()));
    ValueChannelTelemetryEventBatch batch;
    REQUIRE(attachment.try_pop_events(0, batch));
    REQUIRE(batch.frame.count == EventFrame::kMaxEvents);
    CHECK(batch.frame.events[EventFrame::kMaxEvents - 1].value ==
          Catch::Approx(static_cast<float>(EventFrame::kMaxEvents - 1)));

    const auto capacity = attachment.event_stats(0).capacity;
    REQUIRE(capacity > 0);
    const ValueEvent event{.frame_index = 0, .value = 1.0f};
    for (std::size_t i = 0; i < capacity + 3; ++i)
        events->publish(&event, 1);
    const auto full = attachment.event_stats(0);
    CHECK(full.size_approx == capacity);
    CHECK(full.overflow_count == 3);
    for (std::size_t i = 0; i < capacity; ++i)
        REQUIRE(attachment.try_pop_events(0, batch));
    CHECK_FALSE(attachment.try_pop_events(0, batch));
}

TEST_CASE("active telemetry keeps producer and UI paths allocation-free",
          "[view][value-channel][telemetry][rt-safety]") {
    ValueChannelSet channels;
    auto* scalar = channels.declare_scalar("scalar");
    auto* meter = channels.declare_meter("meter");
    auto* vector = channels.declare_vector("vector");
    auto* events = channels.declare_events("events");
    auto attachment = channels.attach_telemetry();
    REQUIRE(attachment.valid());

    const auto meter_frame = stereo(0.2f, 0.4f);
    std::array<float, 64> samples{};
    const ValueEvent occurrence{.frame_index = 3, .value = 1.0f};
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 64; ++i) {
            scalar->publish(static_cast<float>(i));
            meter->publish(meter_frame);
            vector->publish(samples.data(), static_cast<int>(samples.size()));
            events->publish(&occurrence, 1);
            scalar->read();
            meter->read();
            vector->read();
        }
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
}

TEST_CASE("continuous telemetry remains coherent across its three thread roles",
          "[view][value-channel][telemetry][thread][rt-safety]") {
    ValueChannelSet channels;
    auto* vector = channels.declare_vector("spectrum");
    auto attachment = channels.attach_telemetry();
    constexpr int publications = 4000;
    constexpr int width = 64;
    std::atomic<bool> writer_done{false};
    std::atomic<bool> ui_done{false};
    std::atomic<int> ui_torn{0};

    std::thread writer([&] {
        std::array<float, width> payload{};
        for (int publication = 1; publication <= publications; ++publication) {
            for (int i = 0; i < width; ++i)
                payload[static_cast<std::size_t>(i)] = publication + i;
            vector->publish(payload.data(), width);
        }
        writer_done.store(true, std::memory_order_release);
    });
    std::thread ui([&] {
        int final_reads = 0;
        while (!writer_done.load(std::memory_order_acquire) || final_reads < 128) {
            const auto frame = vector->read();
            if (frame.count == width) {
                const float base = frame.samples[0];
                for (int i = 1; i < width; ++i) {
                    if (frame.samples[static_cast<std::size_t>(i)] != base + i) {
                        ui_torn.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
            if (writer_done.load(std::memory_order_acquire))
                ++final_reads;
        }
        ui_done.store(true, std::memory_order_release);
    });

    int broker_torn = 0;
    int broker_reads = 0;
    while (!ui_done.load(std::memory_order_acquire) || broker_reads < 128) {
        ValueChannelTelemetrySnapshot snapshot;
        if (!attachment.read_continuous(0, snapshot) || !snapshot.available)
            continue;
        const auto& payload = std::get<VectorFrame>(snapshot.payload);
        if (payload.count != width)
            continue;
        const float base = payload.samples[0];
        if (base != static_cast<float>(snapshot.source_publication))
            ++broker_torn;
        for (int i = 1; i < width; ++i) {
            if (payload.samples[static_cast<std::size_t>(i)] != base + i) {
                ++broker_torn;
                break;
            }
        }
        ++broker_reads;
    }
    writer.join();
    ui.join();
    CHECK(ui_torn.load() == 0);
    CHECK(broker_torn == 0);
    CHECK(broker_reads >= 128);
}

TEST_CASE("event telemetry remains ordered with a concurrent producer and broker",
          "[view][value-channel][telemetry][thread][rt-safety]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    auto attachment = channels.attach_telemetry();
    constexpr std::uint64_t publications = 20000;
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (std::uint64_t publication = 1; publication <= publications;
             ++publication) {
            const ValueEvent occurrence{
                .frame_index = static_cast<std::uint32_t>(publication),
                .value = static_cast<float>(publication)};
            events->publish(&occurrence, 1);
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t delivered = 0;
    std::uint64_t previous_publication = 0;
    const auto consume = [&](const ValueChannelTelemetryEventBatch& batch) {
        REQUIRE(batch.frame.count == 1);
        CHECK(batch.source_publication > previous_publication);
        CHECK(batch.frame.publication == batch.source_publication);
        CHECK(batch.frame.events[0].value ==
              Catch::Approx(static_cast<float>(batch.source_publication)));
        previous_publication = batch.source_publication;
        ++delivered;
    };
    while (!producer_done.load(std::memory_order_acquire)) {
        ValueChannelTelemetryEventBatch batch;
        if (attachment.try_pop_events(0, batch))
            consume(batch);
    }
    producer.join();
    ValueChannelTelemetryEventBatch batch;
    while (attachment.try_pop_events(0, batch))
        consume(batch);
    const auto stats = attachment.event_stats(0);
    CHECK(delivered + stats.overflow_count == publications);
}

TEST_CASE("reattaching event telemetry discards batches from the prior reader",
          "[view][value-channel][telemetry][lifecycle]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    const ValueEvent first{.frame_index = 1, .value = 1.0f};
    {
        auto first_attachment = channels.attach_telemetry();
        events->publish(&first, 1);
    }

    auto second_attachment = channels.attach_telemetry();
    ValueChannelTelemetryEventBatch batch;
    CHECK_FALSE(second_attachment.try_pop_events(0, batch));
    const ValueEvent second{.frame_index = 2, .value = 2.0f};
    events->publish(&second, 1);
    REQUIRE(second_attachment.try_pop_events(0, batch));
    CHECK(batch.frame.events[0].frame_index == 2);
}

TEST_CASE("telemetry attachment remains safe after source destruction",
          "[view][value-channel][telemetry][lifecycle]") {
    ValueChannelTelemetryAttachment attachment;
    {
        auto channels = std::make_unique<ValueChannelSet>();
        auto* scalar = channels->declare_scalar("gain_reduction");
        auto* events = channels->declare_events("onsets");
        attachment = channels->attach_telemetry();
        scalar->publish(-2.0f);
        scalar->read();
        const ValueEvent occurrence{.frame_index = 7, .value = 1.0f};
        events->publish(&occurrence, 1);
    }

    REQUIRE(attachment.valid());
    CHECK(attachment.channels()[0].info.name == "gain_reduction");
    CHECK(attachment.channels()[1].info.name == "onsets");
    CHECK_FALSE(attachment.source_alive(0));
    CHECK_FALSE(attachment.source_alive(1));
    REQUIRE(attachment.channels().size() == 2);
    ValueChannelTelemetrySnapshot snapshot;
    REQUIRE(attachment.read_continuous(0, snapshot));
    CHECK(std::get<float>(snapshot.payload) == Catch::Approx(-2.0f));
    ValueChannelTelemetryEventBatch batch;
    REQUIRE(attachment.try_pop_events(1, batch));
    CHECK(batch.frame.events[0].frame_index == 7);
}
