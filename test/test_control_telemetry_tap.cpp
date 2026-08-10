#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_telemetry_tap.hpp>
#include <pulp/view/value_channel_set.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {
ControlTelemetryAuthority authority(std::string client = "client-a") {
    return {.client_id = std::move(client),
            .registration_id = "registration-1",
            .instance_id = "slot-1",
            .grant_id = "grant-1"};
}
} // namespace

TEST_CASE("canonical telemetry tap is disabled by default and preserves sole-reader ownership",
          "[inspect][control][telemetry][security]") {
    pulp::view::ValueChannelSet channels;
    channels.declare_scalar("gain");
    ControlTelemetryTap disabled({});
    CHECK_FALSE(disabled.attach(channels.attach_telemetry()));
    // The failed explicit activation released the exclusive attachment.
    CHECK(channels.attach_telemetry().valid());
}

TEST_CASE("bounded telemetry fanout downsamples redacts and accounts slow subscribers",
          "[inspect][control][telemetry][loss][redaction][soak]") {
    pulp::view::ValueChannelSet channels;
    auto* gain = channels.declare_scalar("gain");
    auto* secret = channels.declare_scalar("auth-token");
    REQUIRE(gain);
    REQUIRE(secret);
    auto now = std::chrono::steady_clock::now();
    ControlTelemetryTap tap({.enabled = true,
                             .maximum_rate_hz = 10.0,
                             .maximum_subscriptions = 2,
                             .maximum_subscriptions_per_client = 1,
                             .maximum_queued_frames = 2},
                            [&] { return now; });
    REQUIRE(tap.attach(channels.attach_telemetry(), [](std::string_view name) {
        return name == "auth-token" ? ControlTelemetrySensitivity::Sensitive
                                    : ControlTelemetrySensitivity::Observable;
    }));
    const auto subscriber = authority();
    const auto subscription =
        tap.subscribe(subscriber, {.channels = {"gain", "auth-token"}, .rate_hz = 100.0});
    REQUIRE(subscription);
    CHECK_FALSE(tap.subscribe(subscriber, {.channels = {"gain"}, .rate_hz = 1.0}));

    for (int value = 1; value <= 5; ++value) {
        gain->publish(static_cast<float>(value));
        secret->publish(999.0f);
        (void)gain->read();
        (void)secret->read();
        tap.poll();
        now += 101ms;
    }
    CHECK_FALSE(tap.try_pop(*subscription, authority("wrong-client")));
    const auto first = tap.try_pop(*subscription, subscriber);
    REQUIRE(first);
    CHECK(first->dropped_since_previous == 3);
    REQUIRE(first->channels.size() == 2);
    CHECK(first->channels[0].values.front() == Catch::Approx(4.0f));
    CHECK(first->channels[1].redacted);
    CHECK(first->channels[1].channel != "auth-token");
    CHECK(first->channels[1].values.empty());
    const auto second = tap.try_pop(*subscription, subscriber);
    REQUIRE(second);
    CHECK(second->dropped_since_previous == 0);
}

TEST_CASE("telemetry without a classifier fails closed",
          "[inspect][control][telemetry][security][redaction]") {
    pulp::view::ValueChannelSet channels;
    auto* value = channels.declare_scalar("unclassified");
    REQUIRE(value);
    ControlTelemetryTap tap({.enabled = true});
    REQUIRE(tap.attach(channels.attach_telemetry()));
    const auto subscriber = authority();
    CHECK_FALSE(tap.subscribe(subscriber, {.channels = {"unclassified"}, .rate_hz = 1.0e-300}));
    const auto subscription = tap.subscribe(subscriber, {.channels = {"unclassified"}});
    REQUIRE(subscription);
    value->publish(42.0f);
    (void)value->read();
    tap.poll();
    const auto frame = tap.try_pop(*subscription, subscriber);
    REQUIRE(frame);
    REQUIRE(frame->channels.size() == 1);
    CHECK(frame->channels.front().redacted);
    CHECK(frame->channels.front().values.empty());
}

TEST_CASE("sensitive telemetry names and values require explicit authority",
          "[inspect][control][telemetry][security][redaction]") {
    pulp::view::ValueChannelSet channels;
    auto* secret = channels.declare_scalar("secret-level");
    REQUIRE(secret);
    ControlTelemetryTap tap({.enabled = true});
    REQUIRE(tap.attach(channels.attach_telemetry(),
                       [](std::string_view) { return ControlTelemetrySensitivity::Sensitive; }));
    auto subscriber = authority();
    subscriber.allow_sensitive = true;
    const auto subscription = tap.subscribe(subscriber, {.channels = {"secret-level"}});
    REQUIRE(subscription);
    secret->publish(7.0f);
    (void)secret->read();
    tap.poll();
    const auto frame = tap.try_pop(*subscription, subscriber);
    REQUIRE(frame);
    REQUIRE(frame->channels.size() == 1);
    CHECK(frame->channels.front().channel == "secret-level");
    CHECK_FALSE(frame->channels.front().redacted);
    REQUIRE(frame->channels.front().values.size() == 1);
    CHECK(frame->channels.front().values.front() == Catch::Approx(7.0f));
}

TEST_CASE("slow telemetry subscribers retain event batches until their next sample",
          "[inspect][control][telemetry][events][downsampling]") {
    pulp::view::ValueChannelSet channels;
    auto* events = channels.declare_events("hits");
    REQUIRE(events);
    auto now = std::chrono::steady_clock::now();
    ControlTelemetryTap tap({.enabled = true, .maximum_rate_hz = 10.0}, [&] { return now; });
    REQUIRE(tap.attach(channels.attach_telemetry(),
                       [](std::string_view) { return ControlTelemetrySensitivity::Observable; }));
    const auto subscriber = authority();
    const auto subscription = tap.subscribe(
        subscriber, {.channels = {"hits"}, .rate_hz = 1.0, .maximum_vector_values = 8});
    REQUIRE(subscription);
    tap.poll();
    REQUIRE(tap.try_pop(*subscription, subscriber));

    const std::array<pulp::view::ValueEvent, 3> event{{
        {.frame_index = 1, .value = 1.0f},
        {.frame_index = 2, .value = 1.0f},
        {.frame_index = 3, .value = 1.0f},
    }};
    events->publish(event.data(), static_cast<int>(event.size()));
    now += 100ms;
    tap.poll();
    CHECK_FALSE(tap.try_pop(*subscription, subscriber));
    now += 900ms;
    tap.poll();
    const auto frame = tap.try_pop(*subscription, subscriber);
    REQUIRE(frame);
    REQUIRE(frame->channels.size() == 1);
    REQUIRE(frame->channels.front().values.size() == 1);
    CHECK(frame->channels.front().values.front() == Catch::Approx(3.0f));
}

TEST_CASE("telemetry producer and slow subscriber sustain bounded high-iteration soak",
          "[inspect][control][telemetry][soak][watchdog][loss]") {
    constexpr std::size_t kPolls = 10'000;
    constexpr std::size_t kPublications = 100'000;
    constexpr std::size_t kQueueCapacity = 4;
    auto channels = std::make_shared<pulp::view::ValueChannelSet>();
    auto* gain = channels->declare_scalar("gain");
    REQUIRE(gain);
    auto now = std::chrono::steady_clock::now();
    ControlTelemetryTap tap({.enabled = true,
                             .maximum_rate_hz = 60.0,
                             .maximum_queued_frames = kQueueCapacity},
                            [&] { return now; });
    REQUIRE(tap.attach(channels->attach_telemetry(), [](std::string_view) {
        return ControlTelemetrySensitivity::Observable;
    }));
    const auto subscriber = authority();
    const auto subscription =
        tap.subscribe(subscriber, {.channels = {"gain"}, .rate_hz = 60.0});
    REQUIRE(subscription);

    struct ProducerState {
        std::atomic<bool> finished{false};
        std::atomic<std::uint64_t> produced{0};
    };
    const auto producer_state = std::make_shared<ProducerState>();
    const auto watchdog_deadline = std::chrono::steady_clock::now() + 5s;
    std::thread producer([channels, gain, producer_state] {
        (void)channels; // Own the channel storage until a detached timeout exits.
        for (std::size_t index = 0; index < kPublications; ++index) {
            gain->publish(static_cast<float>(index));
            (void)gain->read();
            producer_state->produced.fetch_add(1, std::memory_order_relaxed);
        }
        producer_state->finished.store(true, std::memory_order_release);
    });

    for (std::size_t poll = 0; poll < kPolls; ++poll) {
        tap.poll();
        now += 17ms;
    }
    while (!producer_state->finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < watchdog_deadline)
        std::this_thread::yield();
    if (!producer_state->finished.load(std::memory_order_acquire)) {
        producer.detach();
        FAIL_CHECK("telemetry producer exceeded the five-second render/audio watchdog");
        return;
    }
    producer.join();

    CHECK(producer_state->finished.load(std::memory_order_acquire));
    CHECK(producer_state->produced.load(std::memory_order_relaxed) == kPublications);
    CHECK(std::chrono::steady_clock::now() < watchdog_deadline);
    std::size_t retained_frames = 0;
    std::size_t retained_values = 0;
    std::uint64_t reported_loss = 0;
    while (const auto frame = tap.try_pop(*subscription, subscriber)) {
        ++retained_frames;
        reported_loss += frame->dropped_since_previous;
        for (const auto& channel : frame->channels)
            retained_values += channel.values.size();
    }
    CHECK(retained_frames == kQueueCapacity);
    CHECK(retained_values <= kQueueCapacity);
    CHECK(reported_loss == kPolls - kQueueCapacity);
}
